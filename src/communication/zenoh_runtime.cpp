#include "gateway.hpp"
#include <zenoh.h>
#include <iostream>
#include <limits>
#include <openssl/rand.h>

namespace poke::log {
namespace {
std::uint64_t process_epoch() {
    std::uint64_t epoch{};
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&epoch), sizeof(epoch)) != 1)
            throw Error("INTERNAL", "cannot generate log service epoch");
    } while (!epoch);
    return epoch;
}
}
struct LogGateway::Runtime {
    z_owned_session_t session;
    z_owned_subscriber_t requests, acknowledgements;
    std::string prefix, producer_id;
    std::uint64_t epoch, sequence{};
    std::mutex send_mutex;
    std::function<void(Message)> receiver;
    explicit Runtime(const Config& config)
        : prefix("poke/pcu/" + config.pcu_id + "/logs/"), producer_id(config.pcu_id), epoch(process_epoch()) {
        z_internal_null(&session); z_internal_null(&requests); z_internal_null(&acknowledgements);
        z_owned_config_t options;
        if (zc_config_from_file(&options, config.zenoh_config.c_str()) < 0)
            throw Error("ZENOH", "cannot read Zenoh configuration");
        if (z_open(&session, z_move(options), nullptr) < 0) throw Error("ZENOH", "cannot open session");
    }
    ~Runtime() {
        stop();
        if (z_internal_check(session)) z_drop(z_move(session));
    }
    static void sample(z_loaned_sample_t* sample, void* context) noexcept {
        auto& self = *static_cast<Runtime*>(context);
        try {
            const auto* payload = z_sample_payload(sample);
            const auto size = z_bytes_len(payload);
            if (size > 16384) throw Error("BAD_MESSAGE", "inbound payload exceeds 16 KiB");
            Bytes bytes(size);
            auto reader = z_bytes_get_reader(payload);
            if (z_bytes_reader_read(&reader, bytes.data(), bytes.size()) != bytes.size())
                throw Error("BAD_MESSAGE", "short payload");
            auto message = LogCodec::decode(bytes);
            z_view_string_t key;
            z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
            const std::string actual(z_string_data(z_loan(key)), z_string_len(z_loan(key)));
            const auto expected = self.prefix + (message.kind == Kind::Get ? "get" : "ack");
            if ((message.kind != Kind::Get && message.kind != Kind::Ack) || actual != expected)
                throw Error("BAD_MESSAGE", "message kind does not match endpoint");
            self.receiver(std::move(message));
        } catch (const std::exception& e) { std::cerr << "request rejected: " << e.what() << '\n'; }
    }
    void subscribe(z_owned_subscriber_t& sub, const std::string& key) {
        z_view_keyexpr_t expr;
        if (z_view_keyexpr_from_str(&expr, key.c_str()) < 0) throw Error("ZENOH", "invalid key");
        z_owned_closure_sample_t callback;
        z_closure(&callback, sample, nullptr, this);
        if (z_declare_subscriber(z_loan(session), &sub, z_loan(expr), z_move(callback), nullptr) < 0)
            throw Error("ZENOH", "cannot declare subscriber");
    }
    void stop() {
        if (z_internal_check(requests)) z_drop(z_move(requests));
        if (z_internal_check(acknowledgements)) z_drop(z_move(acknowledgements));
    }
};
LogGateway::LogGateway(const Config& c) : runtime_(std::make_unique<Runtime>(c)) {}
LogGateway::~LogGateway() = default;
void LogGateway::start(std::function<void(Message)> receiver) {
    runtime_->receiver = std::move(receiver);
    try {
        runtime_->subscribe(runtime_->requests, runtime_->prefix + "get");
        runtime_->subscribe(runtime_->acknowledgements, runtime_->prefix + "ack");
    } catch (...) {
        runtime_->stop();
        throw;
    }
}
void LogGateway::stop_receiving() { runtime_->stop(); }
void LogGateway::send(const Message& m) {
    // Serialize numbering and publication together, including concurrent BUSY/ERROR replies.
    // A process-wide counter gives every request a strictly increasing subsequence.
    std::lock_guard<std::mutex> lock(runtime_->send_mutex);
    if (runtime_->sequence == std::numeric_limits<std::uint64_t>::max())
        throw Error("TRANSFER", "log sequence exhausted; restart service with a new epoch");
    auto outgoing = m;
    outgoing.header = {};
    outgoing.header.producer_id = runtime_->producer_id;
    outgoing.header.stream_epoch = runtime_->epoch;
    outgoing.header.sequence = ++runtime_->sequence;
    outgoing.header.timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    outgoing.header.clock_domain = ClockDomain::UnixEpoch;
    const auto payload = LogCodec::encode(outgoing);
    const auto key = runtime_->prefix + "result/" + m.request_id;
    z_view_keyexpr_t expr;
    if (z_view_keyexpr_from_str(&expr, key.c_str()) < 0) throw Error("ZENOH", "invalid response key");
    z_owned_bytes_t bytes;
    if (z_bytes_copy_from_buf(&bytes, payload.data(), payload.size()) < 0) throw Error("ZENOH", "cannot allocate payload");
    z_put_options_t options;
    z_put_options_default(&options);
    options.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    if (z_put(z_loan(runtime_->session), z_loan(expr), z_move(bytes), &options) < 0)
        throw Error("ZENOH", "publish failed");
}
}
