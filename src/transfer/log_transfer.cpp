#include <poke/log/service.hpp>
#include <openssl/evp.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace poke::log {
std::string sha256_file(const fs::path& path) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) throw Error("HASH", "SHA-256 init failed");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("HASH", "cannot open archive");
    std::array<char, 65536> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(in.gcount())) != 1)
            throw Error("HASH", "SHA-256 update failed");
    }
    if (!in.eof()) throw Error("HASH", "archive read failed");
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int size{};
    if (EVP_DigestFinal_ex(ctx.get(), hash, &size) != 1) throw Error("HASH", "SHA-256 final failed");
    std::ostringstream result;
    for (unsigned int i = 0; i < size; ++i) result << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);
    return result.str();
}
Message LogTransfer::describe(const Message& request, const fs::path& archive) const {
    Message m;
    m.request_id = request.request_id;
    m.kind = Kind::Begin;
    m.file_name = "logs-" + request.request_id + ".tar.gz";
    m.file_size = fs::file_size(archive);
    m.chunk_size = config_.chunk_size;
    m.chunk_count = static_cast<std::uint32_t>((m.file_size + m.chunk_size - 1) / m.chunk_size);
    m.sha256 = sha256_file(archive);
    return m;
}
void LogTransfer::send(const Message& metadata, const fs::path& archive, const Cancelled& cancelled) {
    transport_.send(metadata);
    std::ifstream in(archive, std::ios::binary);
    if (!in) throw Error("TRANSFER", "cannot open archive");
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t sent{};
    for (std::uint32_t index = 0; index < metadata.chunk_count; ++index) {
        if (cancelled()) throw Error("CANCELLED", "service stopping");
        Message chunk;
        chunk.kind = Kind::Chunk;
        chunk.request_id = metadata.request_id;
        chunk.chunk_index = index;
        chunk.data.resize(static_cast<std::size_t>(std::min<std::uint64_t>(metadata.chunk_size, metadata.file_size - sent)));
        in.read(reinterpret_cast<char*>(chunk.data.data()), static_cast<std::streamsize>(chunk.data.size()));
        if (static_cast<std::size_t>(in.gcount()) != chunk.data.size()) throw Error("TRANSFER", "short archive read");
        transport_.send(chunk);
        sent += chunk.data.size();
        const auto deadline = start + std::chrono::microseconds(sent * 1000000 / config_.bytes_per_second);
        while (!cancelled() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto end = metadata;
    end.kind = Kind::End;
    transport_.send(end);
}
}
