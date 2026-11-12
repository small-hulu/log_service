#include <poke/log/service.hpp>
#include "gateway.hpp"
#include <fstream>
#include <iostream>
#include <mutex>

using namespace poke::log;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class F> void expect(const std::string& code, F&& action) {
    try { action(); } catch (const Error& e) { require(e.code() == code, e.what()); return; }
    throw std::runtime_error("expected error: " + code);
}
int main() {
    fs::path root = fs::current_path() / ("poke-log-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{root};
    try {
        fs::create_directories(root / "source/old"); fs::create_directories(root / "source/current");
        std::ofstream(root / "source/current_run") << "current\n";
        std::ofstream(root / "source/old/a.log") << "old\n";
        std::ofstream(root / "source/current/a.log") << "current\n";
        std::ofstream(root / "source/current/a.1.log") << "rotated\n";
        std::ofstream(root / "secret") << "must not leak";
        fs::create_symlink(root / "secret", root / "source/current/link.log");
        Config c; c.sources = {{"hand", root / "source"}}; c.work_root = root / "work";
        fs::create_directories(c.work_root);
        FileCollector collector(c);
        Message request; request.request_id = "test"; request.mode = Mode::Latest;
        collector.collect(request, root / "latest", [] { return false; });
        require(fs::exists(root / "latest/hand/current/a.1.log"), "latest must include rotated files");
        require(!fs::exists(root / "latest/hand/old"), "latest must exclude prior runs");
        require(!fs::exists(root / "latest/hand/current/link.log"), "symlink leaked");
        request.mode = Mode::All;
        collector.collect(request, root / "all", [] { return false; });
        require(fs::exists(root / "all/hand/old/a.log"), "all missing old run");
        request.modules = {"unknown"};
        expect("UNKNOWN_MODULE", [&] { collector.collect(request, root / "bad", [] { return false; }); });
        request.modules.clear();
        c.max_bytes = 1; FileCollector limited(c);
        expect("LIMIT_EXCEEDED", [&] { limited.collect(request, root / "limited", [] { return false; }); });
        request.mode = Mode::Latest; fs::remove(root / "source/current_run");
        expect("LATEST_UNAVAILABLE", [&] { collector.collect(request, root / "missing", [] { return false; }); });
        request.mode = Mode::All;
        expect("CANCELLED", [&] { collector.collect(request, root / "cancel", [] { return true; }); });
        TarGzipArchiver archiver; archiver.compress(root / "all", root / "logs.tar.gz", [] { return false; });
        require(sha256_file(root / "logs.tar.gz").size() == 64, "missing hash");
        auto encoded = LogCodec::encode(request);
        require(LogCodec::decode(encoded).request_id == request.request_id, "codec round trip");
        encoded.resize(5);
        expect("BAD_MESSAGE", [&] { LogCodec::decode(encoded); });
        std::cout << "PASS: ALL/LATEST, rotated files, symlinks, scope, limits, cancellation, archive, codec\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
