#include <poke/log/service.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace poke::log {
namespace {
class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) { if (fd < 0) throw Error("SOURCE_CHANGED", "cannot open source without following symlinks"); }
    ~Descriptor() { ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    int get() const { return fd_; }
private: int fd_;
};
// Walk from a pinned root descriptor: a renamed directory or symlink cannot
// redirect a collection outside its configured source.
int open_beneath(int root, const fs::path& relative) {
    int parent = ::dup(root);
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        const auto part = it->string();
        if (part == ".." || part == "." || part.empty()) { ::close(parent); return -1; }
        const bool last = std::next(it) == relative.end();
        const int next = ::openat(parent, part.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (last ? 0 : O_DIRECTORY));
        ::close(parent);
        if (next < 0) return -1;
        parent = next;
    }
    return parent;
}
std::string current_run(int root) {
    Descriptor fd(::openat(root, "current_run", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 100)
        throw Error("LATEST_UNAVAILABLE", "invalid current_run marker");
    std::array<char, 101> value{};
    const auto n = ::read(fd.get(), value.data(), 100);
    if (n <= 0) throw Error("LATEST_UNAVAILABLE", "empty current_run marker");
    std::string run(value.data(), static_cast<std::size_t>(n));
    while (!run.empty() && (run.back() == '\n' || run.back() == '\r')) run.pop_back();
    if (!valid_segment(run)) throw Error("LATEST_UNAVAILABLE", "current_run must contain a run directory name");
    return run;
}
}
void FileCollector::collect(const Message& request, const fs::path& output, const Cancelled& cancelled) {
    fs::create_directories(output);
    std::set<std::string> selected(request.modules.begin(), request.modules.end());
    for (const auto& name : selected) {
        if (std::none_of(config_.sources.begin(), config_.sources.end(), [&](const auto& s) { return s.name == name; }))
            throw Error("UNKNOWN_MODULE", "unknown module: " + name);
    }
    nlohmann::json manifest{{"request_id", request.request_id}, {"mode", request.mode == Mode::All ? "ALL" : "LATEST"},
        {"capture_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
        {"snapshot_semantics", "per-file bounded prefix; append-only sources required; no cross-process atomic snapshot"},
        {"files", nlohmann::json::array()}, {"runs", nlohmann::json::object()}};
    std::uint64_t total = 0;
    std::uint32_t count = 0;
    for (const auto& source : config_.sources) {
        if (!selected.empty() && !selected.count(source.name)) continue;
        Descriptor root(::open(source.root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        auto scan = source.root;
        if (request.mode == Mode::Latest) {
            std::string run;
            try { run = current_run(root.get()); }
            catch (const Error&) { throw Error("LATEST_UNAVAILABLE", "module requires valid current_run marker: " + source.name); }
            scan /= run;
            if (!fs::is_directory(fs::symlink_status(scan))) throw Error("LATEST_UNAVAILABLE", "run directory is missing");
            manifest["runs"][source.name] = run;
        }
        for (const auto& entry : fs::recursive_directory_iterator(scan)) {
            if (cancelled()) throw Error("CANCELLED", "service stopping");
            if (!fs::is_regular_file(entry.symlink_status())) continue;
            const auto relative = entry.path().lexically_relative(source.root);
            if (relative == "current_run") continue;
            Descriptor input(open_beneath(root.get(), relative));
            struct stat before{};
            if (::fstat(input.get(), &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || before.st_nlink != 1)
                throw Error("SOURCE_CHANGED", "source must be a regular, non-hardlinked file");
            const auto size = static_cast<std::uint64_t>(before.st_size);
            if (++count > config_.max_files || size > config_.max_bytes - total)
                throw Error("LIMIT_EXCEEDED", "log collection exceeds configured limits");
            total += size;
            const auto destination = output / source.name / relative;
            fs::create_directories(destination.parent_path());
            std::ofstream out(destination, std::ios::binary);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            std::array<char, 65536> buffer{};
            auto remaining = size;
            while (remaining) {
                if (cancelled()) throw Error("CANCELLED", "service stopping");
                const auto n = ::read(input.get(), buffer.data(), std::min<std::uint64_t>(remaining, buffer.size()));
                if (n <= 0) throw Error("SOURCE_CHANGED", "source truncated during capture; retry GET");
                out.write(buffer.data(), n);
                remaining -= static_cast<std::uint64_t>(n);
            }
            out.close();
            struct stat after{};
            if (::fstat(input.get(), &after) || after.st_size < before.st_size)
                throw Error("SOURCE_CHANGED", "source shrank during capture; retry GET");
            manifest["files"].push_back({{"path", (fs::path(source.name)/relative).generic_string()}, {"size", size}});
        }
    }
    if (!count) throw Error("NO_LOGS", "no retained log files in selected scope");
    std::ofstream out(output / "manifest.json");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << manifest.dump(2) << '\n';
}
}
