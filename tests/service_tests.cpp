#include <poke/log/service.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace poke::log;
namespace {
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
Message request(const std::string& id) {
    Message m; m.request_id = id; m.header.producer_id = "sdk-test";
    m.header.stream_epoch = 42; m.header.sequence = 1; return m;
}
Message ack_for(const Message& end, std::uint64_t sequence = 2) {
    auto m = request(end.request_id); m.kind = Kind::Ack; m.header.sequence = sequence;
    m.file_size = end.file_size; m.sha256 = end.sha256; return m;
}
class Collector final : public LogCollector {
public:
    void collect(const Message&, const fs::path& path, const Cancelled&) override {
        fs::create_directories(path); std::ofstream(path / "log") << "test\n";
    }
};
class Transport final : public LogTransport {
public:
    std::function<void(const Message&)> on_send;
    void send(const Message& m) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(m); cv_.notify_all();
        }
        if (on_send) on_send(m);
    }
    Message wait(Kind kind, const std::string& id, const std::string& code = "") {
        std::unique_lock<std::mutex> lock(mutex_);
        Message result;
        const auto found = [&] {
            for (const auto& m : messages_)
                if (m.kind == kind && m.request_id == id && (code.empty() || m.error_code == code)) {
                    result = m; return true;
                }
            return false;
        };
        require(cv_.wait_for(lock, std::chrono::seconds(4), found), "expected response missing");
        return result;
    }
    std::size_t count(Kind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::count_if(messages_.begin(), messages_.end(), [&](const auto& m) { return m.kind == kind; });
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Message> messages_;
};
}
int main() {
    const auto root = fs::current_path() / ("poke-log-service-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
    try {
        Config config; config.work_root = root; config.ack_timeout_seconds = 1;
        Collector collector; TarGzipArchiver archiver;
        const std::vector<std::function<void(Message&)>> mutations = {
            [](Message& m) { m.header.producer_id = "another-sdk"; },
            [](Message& m) { ++m.header.stream_epoch; },
            [](Message& m) { m.header.sequence = 1; },
            [](Message& m) { m.header.sequence = 0; },
            [](Message& m) { m.sha256 = "wrong"; },
            [](Message& m) { ++m.file_size; },
            [](Message& m) { m.request_id = "another-request"; }
        };
        for (std::size_t i = 0; i < mutations.size(); ++i) {
            Transport transport; LogService service(config, collector, archiver, transport);
            auto get = request("invalid-" + std::to_string(i)); service.receive(get);
            const auto end = transport.wait(Kind::End, get.request_id);
            service.receive(get); service.receive(request("busy"));
            transport.wait(Kind::Error, "busy", "BUSY");
            auto ack = ack_for(end); mutations[i](ack); service.receive(ack);
            transport.wait(Kind::Error, get.request_id, "ACK_TIMEOUT"); service.stop();
            require(transport.count(Kind::Begin) == 1, "duplicate caused repeated collection");
            require(transport.count(Kind::Complete) == 0, "invalid ACK accepted");
            require(fs::is_empty(root), "timeout leaked workspace");
        }
        {
            Transport transport; LogService service(config, collector, archiver, transport);
            service.receive(request("replay")); auto end = transport.wait(Kind::End, "replay");
            auto ack = ack_for(end); ack.sha256 = "wrong"; service.receive(ack);
            service.receive(ack_for(end));
            transport.wait(Kind::Error, "replay", "ACK_TIMEOUT"); service.stop();
            require(transport.count(Kind::Complete) == 0, "replayed sequence accepted");
        }
        // Fast SDK: ACK is delivered synchronously from END publication.
        {
            Transport transport; LogService* active{};
            transport.on_send = [&](const Message& m) { if (m.kind == Kind::End) active->receive(ack_for(m)); };
            LogService service(config, collector, archiver, transport); active = &service;
            service.receive(request("fast")); transport.wait(Kind::Complete, "fast"); service.stop();
            require(fs::is_empty(root), "success leaked workspace");
        }
        // An early ACK is consumed but cannot complete; a fresh sequence after END can.
        {
            Transport transport; LogService* active{};
            transport.on_send = [&](const Message& m) {
                if (m.kind == Kind::Begin) active->receive(ack_for(m));
                if (m.kind == Kind::End) active->receive(ack_for(m, 3));
            };
            LogService service(config, collector, archiver, transport); active = &service;
            service.receive(request("early")); transport.wait(Kind::Complete, "early"); service.stop();
        }
        {
            Transport transport; LogService* active{};
            transport.on_send = [&](const Message& m) { if (m.kind == Kind::Begin) active->receive(ack_for(m)); };
            LogService service(config, collector, archiver, transport); active = &service;
            service.receive(request("only-early")); transport.wait(Kind::Error, "only-early", "ACK_TIMEOUT"); service.stop();
            require(transport.count(Kind::Complete) == 0, "early ACK accepted");
        }
        {
            Transport transport; LogService service(config, collector, archiver, transport);
            service.receive(request("cancel")); transport.wait(Kind::End, "cancel"); service.stop();
            require(fs::is_empty(root), "shutdown leaked workspace");
        }
        std::cout << "PASS: identity/epoch/sequence/metadata guards, BUSY, duplicate GET, replay, fast/early ACK, cleanup\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
