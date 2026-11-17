#include <poke/log/service.hpp>
#include <algorithm>
#include <iostream>

namespace poke::log {
namespace {
class Workspace {
public:
    explicit Workspace(const fs::path& root) {
        std::string pattern = (root / "request-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const auto created = ::mkdtemp(name.data());
        if (!created) throw Error("WORKSPACE", "cannot create private request workspace");
        path_ = created;
    }
    ~Workspace() { std::error_code ignored; fs::remove_all(path_, ignored); }
    const fs::path& path() const { return path_; }
private: fs::path path_;
};
}
LogService::LogService(Config config, LogCollector& collector, LogArchiver& archiver, LogTransport& transport)
    : config_(std::move(config)), collector_(collector), archiver_(archiver), transport_(transport),
      transfer_(config_, transport_), worker_([this] { run(); }) {}
LogService::~LogService() { stop(); }
void LogService::stop() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void LogService::fail(const std::string& id, const std::string& code, const std::string& detail) noexcept {
    try {
        Message m; m.kind = Kind::Error; m.request_id = id; m.error_code = code; m.detail = detail;
        transport_.send(m);
    } catch (const std::exception& e) { std::cerr << "error reply failed: " << e.what() << '\n'; }
}
void LogService::receive(Message m) {
    if (!valid_segment(m.request_id) || !m.header.valid() || stopping_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (m.kind == Kind::Ack) {
        if (m.request_id != active_id_ || m.header.producer_id != active_client_.producer_id ||
            m.header.stream_epoch != active_client_.stream_epoch || m.header.sequence <= active_client_.sequence)
            return;
        // Consume a matching stream sequence even when its metadata is wrong.
        // Header matching is a stream guard, not transport authentication.
        active_client_.sequence = m.header.sequence;
        if (awaiting_ack_ && !expected_hash_.empty() && m.sha256 == expected_hash_ && m.file_size == expected_size_) {
            acknowledged_ = true;
            cv_.notify_all();
        }
        return;
    }
    if (m.kind != Kind::Get) return;
    // Active duplicates are ignored: the original transfer is still in flight.
    if (m.request_id == active_id_) {
        if (m.header.producer_id == active_client_.producer_id && m.header.stream_epoch == active_client_.stream_epoch &&
            m.header.sequence > active_client_.sequence)
            active_client_.sequence = m.header.sequence;
        return;
    }
    std::string rejection;
    if (std::find(recent_ids_.begin(), recent_ids_.end(), m.request_id) != recent_ids_.end()) rejection = "DUPLICATE";
    else if (!active_id_.empty()) rejection = "BUSY";
    if (!rejection.empty()) {
        lock.unlock();
        fail(m.request_id, rejection, "use a new request_id after the current request finishes");
        return;
    }
    active_id_ = m.request_id;
    active_client_ = m.header;
    pending_ = std::move(m);
    cv_.notify_all();
}
void LogService::run() {
    while (!stopping_) {
        Message request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
            if (stopping_) break;
            request = std::move(*pending_);
            pending_.reset();
        }
        try { execute(request); }
        catch (const Error& e) { fail(request.request_id, e.code(), e.what()); }
        catch (const std::exception& e) { fail(request.request_id, "INTERNAL", e.what()); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recent_ids_.push_back(request.request_id);
            if (recent_ids_.size() > 128) recent_ids_.pop_front();
            active_id_.clear(); expected_hash_.clear(); expected_size_ = 0;
            active_client_ = {}; awaiting_ack_ = false; acknowledged_ = false;
        }
    }
}
void LogService::execute(const Message& request) {
    Workspace work(config_.work_root);
    const auto snapshot = work.path() / "snapshot";
    const auto archive = work.path() / "logs.tar.gz";
    const Cancelled cancelled = [this] { return stopping_.load(); };
    collector_.collect(request, snapshot, cancelled);
    archiver_.compress(snapshot, archive, cancelled);
    const auto metadata = transfer_.describe(request, archive);
    transfer_.send_data(metadata, archive, cancelled);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        expected_hash_ = metadata.sha256; expected_size_ = metadata.file_size;
        awaiting_ack_ = true;
    }
    // Arm first: a fast SDK can ACK while the END publication is still returning.
    auto end = metadata; end.kind = Kind::End;
    transport_.send(end);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(config_.ack_timeout_seconds), [&] { return acknowledged_ || stopping_; }))
            throw Error("ACK_TIMEOUT", "SDK did not confirm a complete verified archive");
        if (stopping_) throw Error("CANCELLED", "service stopping");
    }
    auto complete = metadata; complete.kind = Kind::Complete;
    transport_.send(complete);
    std::cout << "completed request=" << request.request_id << " bytes=" << metadata.file_size << std::endl;
}
}
