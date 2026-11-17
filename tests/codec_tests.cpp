#include "gateway.hpp"
#include "log_generated.h"
#include <iostream>

using namespace poke::log;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void reject(const Bytes& bytes) {
    try { LogCodec::decode(bytes); }
    catch (const Error& e) { require(e.code() == "BAD_MESSAGE", "unexpected error code"); return; }
    throw std::runtime_error("invalid packet accepted");
}
Bytes packet(const MessageHeader& h, unsigned version = 1, unsigned kind = 0, unsigned mode = 0) {
    flatbuffers::FlatBufferBuilder b;
    const auto producer = b.CreateString(h.producer_id);
    const auto header = poke::protocol::CreateMessageHeader(b, h.protocol_major, h.protocol_minor, producer,
        h.stream_epoch, h.sequence, h.timestamp, static_cast<poke::protocol::TimeUnit>(h.timestamp_unit),
        static_cast<poke::protocol::ClockDomain>(h.clock_domain), h.time_mapping_revision, h.timestamp_uncertainty);
    const auto id = b.CreateString("wire-test");
    poke::logwire::MessageBuilder m(b);
    m.add_request_id(id); m.add_header(header); m.add_version(version);
    m.add_kind(static_cast<poke::logwire::Kind>(kind)); m.add_mode(static_cast<poke::logwire::Mode>(mode));
    poke::logwire::FinishMessageBuffer(b, m.Finish());
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}
}
int main() {
    try {
        MessageHeader good; good.producer_id = "sdk-test"; good.stream_epoch = 42; good.sequence = 1;
        good.timestamp = 123456789; good.clock_domain = ClockDomain::UnixEpoch;
        good.time_mapping_revision = 7; good.timestamp_uncertainty = 99;
        Message m; m.request_id = "round-trip"; m.header = good;
        for (unsigned kind = 0; kind <= 6; ++kind) {
            m.kind = static_cast<Kind>(kind);
            const auto decoded = LogCodec::decode(LogCodec::encode(m));
            require(decoded.kind == m.kind && decoded.header.producer_id == good.producer_id &&
                decoded.header.stream_epoch == 42 && decoded.header.sequence == 1 &&
                decoded.header.timestamp == good.timestamp && decoded.header.clock_domain == ClockDomain::UnixEpoch &&
                decoded.header.time_mapping_revision == 7 && decoded.header.timestamp_uncertainty == 99, "header round trip");
        }
        auto bad = good; bad.protocol_major = 1; reject(packet(bad));
        bad = good; bad.protocol_minor = 1; reject(packet(bad));
        bad = good; bad.producer_id.clear(); reject(packet(bad));
        bad = good; bad.producer_id = std::string("sdk\0bad", 7); reject(packet(bad));
        bad = good; bad.stream_epoch = 0; reject(packet(bad));
        bad = good; bad.sequence = 0; reject(packet(bad));
        bad = good; bad.timestamp_unit = static_cast<TimeUnit>(255); reject(packet(bad));
        bad = good; bad.clock_domain = static_cast<ClockDomain>(255); reject(packet(bad));
        reject(packet(good, 2)); reject(packet(good, 1, 7)); reject(packet(good, 1, 0, 2));
        auto missing = packet(good);
        auto* table = missing.data() + flatbuffers::ReadScalar<flatbuffers::uoffset_t>(missing.data());
        auto* vtable = table - flatbuffers::ReadScalar<flatbuffers::soffset_t>(table);
        flatbuffers::WriteScalar<flatbuffers::voffset_t>(vtable + poke::logwire::Message::VT_HEADER, 0);
        reject(missing);
        auto bytes = packet(good); bytes[4] = 'X'; reject(bytes);
        bytes = packet(good); bytes.resize(bytes.size() / 2); reject(bytes);
        for (std::size_t size = 0; size < 8; ++size) reject(Bytes(size));
        std::cout << "PASS: seven message kinds, required header, identity, versions, enums, corrupt buffers\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
