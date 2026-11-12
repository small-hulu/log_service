#include <poke/log/service.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace poke::log;
namespace {
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
class Collector final : public LogCollector {
public:
    void collect(const Message&, const fs::path& path, const Cancelled&) override {
        fs::create_directories(path); std::ofstream(path / "log") << "test\n";
    }
};
class Transport final : public LogTransport {
public:
    void send(const Message& m) override {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(m); cv.notify_all();
    }
    Message wait(Kind kind, const std::string& id, const std::string& code = "") {
        std::unique_lock<std::mutex> lock(mutex);
        Message result;
        const auto found = [&] {
            for (const auto& m : messages) if (m.kind == kind && m.request_id == id && (code.empty() || m.error_code == code)) { result = m; return true; }
            return false;
        };
        require(cv.wait_for(lock, std::chrono::seconds(4), found), "expected response missing");
        return result;
    }
    std::size_t count(Kind kind) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::count_if(messages.begin(), messages.end(), [&](const auto& m) { return m.kind == kind; });
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Message> messages;
};
}
int main() {
    const auto root = fs::current_path() / ("poke-log-service-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
    try {
        Config config; config.work_root = root; config.ack_timeout_seconds = 1;
        Collector collector; TarGzipArchiver archiver; Transport transport;
        LogService service(config, collector, archiver, transport);
        Message first; first.request_id = "first";
        service.receive(first);
        const auto metadata = transport.wait(Kind::End, "first");
        service.receive(first);
        Message second; second.request_id = "second"; service.receive(second);
        transport.wait(Kind::Error, "second", "BUSY");
        Message ack; ack.kind = Kind::Ack; ack.request_id = "first";
        ack.file_size = metadata.file_size; ack.sha256 = "wrong"; service.receive(ack);
        transport.wait(Kind::Error, "first", "ACK_TIMEOUT");
        require(transport.count(Kind::Begin) == 1, "duplicate caused repeated collection");
        require(transport.count(Kind::Complete) == 0, "wrong ACK accepted");
        service.stop();
        require(fs::is_empty(root), "timeout leaked workspace");
        Transport success;
        LogService next(config, collector, archiver, success);
        first.request_id = "success"; next.receive(first);
        const auto end = success.wait(Kind::End, "success");
        ack.request_id = "success"; ack.file_size = end.file_size; ack.sha256 = end.sha256;
        next.receive(ack); success.wait(Kind::Complete, "success"); next.stop();
        require(fs::is_empty(root), "success leaked workspace");
        Transport cancellation;
        LogService last(config, collector, archiver, cancellation);
        first.request_id = "cancel"; last.receive(first);
        cancellation.wait(Kind::End, "cancel"); last.stop();
        require(fs::is_empty(root), "shutdown leaked workspace");
        std::cout << "PASS: BUSY, active duplicate, wrong ACK, timeout, confirmed delivery, shutdown cleanup\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
