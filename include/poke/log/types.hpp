#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace poke::log {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
enum class Mode { All, Latest };
enum class Kind { Get, Begin, Chunk, End, Ack, Complete, Error };
enum class ClockDomain : std::uint8_t { Unspecified, UnixEpoch, HostMonotonic, DeviceMonotonic };
enum class TimeUnit : std::uint8_t { Nanoseconds, Microseconds, Milliseconds };
// Transport-neutral representation of the shared protocol envelope.
struct MessageHeader {
    std::uint16_t protocol_major{2}, protocol_minor{};
    std::string producer_id;
    std::uint64_t stream_epoch{}, sequence{}, timestamp{};
    TimeUnit timestamp_unit{TimeUnit::Nanoseconds};
    ClockDomain clock_domain{ClockDomain::Unspecified};
    std::uint64_t time_mapping_revision{}, timestamp_uncertainty{};
    bool valid() const noexcept {
        return protocol_major == 2 && protocol_minor == 0 && !producer_id.empty() &&
            producer_id.find('\0') == std::string::npos &&
            stream_epoch != 0 && sequence != 0 &&
            timestamp_unit <= TimeUnit::Milliseconds && clock_domain <= ClockDomain::DeviceMonotonic;
    }
};
struct Message {
    Kind kind{Kind::Get};
    std::string request_id;
    Mode mode{Mode::All};
    std::vector<std::string> modules;
    std::string file_name;
    std::uint64_t file_size{};
    std::uint32_t chunk_size{}, chunk_count{}, chunk_index{};
    std::string sha256;
    Bytes data;
    std::string error_code, detail;
    MessageHeader header;
};
struct Source { std::string name; fs::path root; };
struct Config {
    std::string pcu_id;
    fs::path zenoh_config, work_root;
    std::vector<Source> sources;
    std::uint64_t max_bytes{256ULL * 1024 * 1024};
    std::uint32_t max_files{10000}, chunk_size{65536}, ack_timeout_seconds{30};
    std::uint64_t bytes_per_second{4ULL * 1024 * 1024};
};
class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& detail)
        : std::runtime_error(detail), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};
bool valid_segment(const std::string& value);
Config load_config(const fs::path& path);
std::string sha256_file(const fs::path& path);
}
