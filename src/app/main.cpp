#include <poke/log/service.hpp>
#include "gateway.hpp"
#include <csignal>
#include <iostream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
class InstanceLock {
public:
    explicit InstanceLock(const std::filesystem::path& path) {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) throw std::runtime_error("cannot open instance lock");
        if (::flock(fd_, LOCK_EX | LOCK_NB)) { ::close(fd_); throw std::runtime_error("another instance uses this work directory"); }
    }
    ~InstanceLock() { ::close(fd_); }
private: int fd_;
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) { std::cerr << "usage: log_service CONFIG.json\n"; return 2; }
        auto config = poke::log::load_config(argv[1]);
        InstanceLock instance(config.work_root / ".lock");
        poke::log::FileCollector collector(config);
        poke::log::TarGzipArchiver archiver;
        poke::log::LogGateway gateway(config);
        poke::log::LogService service(config, collector, archiver, gateway);
        gateway.start([&](auto message) { service.receive(std::move(message)); });
        std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
        std::cout << "log_service ready pcu=" << config.pcu_id << std::endl;
        while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gateway.stop_receiving();
        service.stop();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
