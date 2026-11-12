#include <poke/log/types.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <set>

namespace poke::log {
bool valid_segment(const std::string& s) {
    return !s.empty() && s.size() <= 96 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
Config load_config(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw Error("CONFIG", "cannot open configuration");
    const auto j = nlohmann::json::parse(in);
    Config c;
    c.pcu_id = j.at("pcu_id").get<std::string>();
    c.zenoh_config = j.at("zenoh_config").get<std::string>();
    c.work_root = j.at("work_root").get<std::string>();
    c.max_bytes = j.value("max_bytes", c.max_bytes);
    c.max_files = j.value("max_files", c.max_files);
    c.chunk_size = j.value("chunk_size", c.chunk_size);
    c.bytes_per_second = j.value("bytes_per_second", c.bytes_per_second);
    c.ack_timeout_seconds = j.value("ack_timeout_seconds", c.ack_timeout_seconds);
    if (!valid_segment(c.pcu_id) || !c.work_root.is_absolute() || !c.zenoh_config.is_absolute() ||
        c.chunk_size < 1024 || c.chunk_size > 65536 || c.max_files == 0 || c.max_files > 100000 ||
        c.max_bytes == 0 || c.max_bytes > 4ULL*1024*1024*1024 ||
        c.ack_timeout_seconds == 0 || c.ack_timeout_seconds > 300 ||
        c.bytes_per_second < c.chunk_size || c.bytes_per_second > 100ULL*1024*1024)
        throw Error("CONFIG", "invalid identity, absolute paths or resource limits");
    std::set<std::string> names;
    for (const auto& v : j.at("sources")) {
        Source source{v.at("name"), v.at("root").get<std::string>()};
        if (!valid_segment(source.name) || !names.insert(source.name).second || !source.root.is_absolute())
            throw Error("CONFIG", "source names must be unique; roots must be absolute");
        source.root = fs::canonical(source.root);
        c.sources.push_back(std::move(source));
    }
    if (c.sources.empty()) throw Error("CONFIG", "at least one source is required");
    fs::create_directories(c.work_root);
    c.work_root = fs::canonical(c.work_root);
    fs::permissions(c.work_root, fs::perms::owner_all);
    for (const auto& source : c.sources) {
        const auto relative = c.work_root.lexically_relative(source.root);
        if (relative.empty() || *relative.begin() != "..")
            throw Error("CONFIG", "work_root must not be inside a source directory");
    }
    return c;
}
}
