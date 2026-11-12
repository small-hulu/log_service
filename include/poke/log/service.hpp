#pragma once
#include "types.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace poke::log {
using Cancelled = std::function<bool()>;
// Transport-neutral boundaries: core tests never need Zenoh or FlatBuffers.
class LogCollector {
public:
    virtual ~LogCollector() = default;
    virtual void collect(const Message&, const fs::path&, const Cancelled&) = 0;
};
class LogArchiver {
public:
    virtual ~LogArchiver() = default;
    virtual void compress(const fs::path&, const fs::path&, const Cancelled&) = 0;
};
class LogTransport {
public:
    virtual ~LogTransport() = default;
    virtual void send(const Message&) = 0;
};
class FileCollector final : public LogCollector {
public:
    explicit FileCollector(Config config) : config_(std::move(config)) {}
    void collect(const Message&, const fs::path&, const Cancelled&) override;
private:
    Config config_;
};
class TarGzipArchiver final : public LogArchiver {
public:
    void compress(const fs::path&, const fs::path&, const Cancelled&) override;
};
class LogTransfer {
public:
    LogTransfer(const Config& config, LogTransport& transport)
        : config_(config), transport_(transport) {}
    Message describe(const Message& request, const fs::path& archive) const;
    void send(const Message& metadata, const fs::path&, const Cancelled&);
private:
    const Config& config_;
    LogTransport& transport_;
};
class LogService {
public:
    LogService(Config, LogCollector&, LogArchiver&, LogTransport&);
    ~LogService();
    LogService(const LogService&) = delete;
    LogService& operator=(const LogService&) = delete;
    void receive(Message); // bounded callback work: never collect/compress here
    void stop();
private:
    void run();
    void execute(const Message&);
    void fail(const std::string&, const std::string&, const std::string&) noexcept;
    Config config_;
    LogCollector& collector_;
    LogArchiver& archiver_;
    LogTransport& transport_;
    LogTransfer transfer_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Message> pending_;
    std::string active_id_, expected_hash_;
    std::uint64_t expected_size_{};
    bool acknowledged_{};
    std::deque<std::string> recent_ids_;
    std::thread worker_;
};
}
