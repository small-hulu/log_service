#include <poke/log/service.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <fstream>
#include <memory>

namespace poke::log {
void TarGzipArchiver::compress(const fs::path& source, const fs::path& destination, const Cancelled& cancelled) {
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    if (!writer) throw Error("ARCHIVE", "cannot create archive writer");
    const auto check = [&](int result) {
        if (result < ARCHIVE_OK) throw Error("ARCHIVE", archive_error_string(writer.get()) ? archive_error_string(writer.get()) : "archive operation failed");
    };
    check(archive_write_add_filter_gzip(writer.get()));
    check(archive_write_set_format_pax_restricted(writer.get()));
    check(archive_write_open_filename(writer.get(), destination.c_str()));
    std::array<char, 65536> buffer{};
    for (const auto& file : fs::recursive_directory_iterator(source)) {
        if (!file.is_regular_file()) continue;
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        const auto relative = file.path().lexically_relative(source).generic_string();
        archive_entry_set_pathname(entry.get(), relative.c_str());
        archive_entry_set_size(entry.get(), static_cast<la_int64_t>(file.file_size()));
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0600);
        check(archive_write_header(writer.get(), entry.get()));
        std::ifstream in(file.path(), std::ios::binary);
        if (!in) throw Error("ARCHIVE", "snapshot file unavailable");
        while (in) {
            if (cancelled()) throw Error("CANCELLED", "service stopping");
            in.read(buffer.data(), buffer.size());
            const auto n = in.gcount();
            if (n && archive_write_data(writer.get(), buffer.data(), n) != n)
                throw Error("ARCHIVE", "cannot write archive data");
        }
        if (!in.eof()) throw Error("ARCHIVE", "snapshot read failed");
        check(archive_write_finish_entry(writer.get()));
    }
    check(archive_write_close(writer.get()));
}
}
