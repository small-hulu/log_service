#pragma once
#include <poke/log/service.hpp>
#include <memory>

namespace poke::log {
class LogCodec {
public:
    static Bytes encode(const Message&);
    static Message decode(const Bytes&);
};
// Pimpl keeps the zenoh-c ABI out of application and business headers.
class LogGateway final : public LogTransport {
public:
    explicit LogGateway(const Config&);
    ~LogGateway();
    void start(std::function<void(Message)> receiver);
    void stop_receiving();
    void send(const Message&) override;
private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
}
