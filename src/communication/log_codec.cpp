#include "gateway.hpp"
#include "log_generated.h"

namespace poke::log {
Bytes LogCodec::encode(const Message& m) {
    if (!m.header.valid() || !valid_segment(m.request_id))
        throw Error("BAD_MESSAGE", "invalid log header or request_id");
    flatbuffers::FlatBufferBuilder b;
    const auto producer = b.CreateString(m.header.producer_id);
    const auto header = protocol::CreateMessageHeader(b, m.header.protocol_major, m.header.protocol_minor,
        producer, m.header.stream_epoch, m.header.sequence, m.header.timestamp,
        static_cast<protocol::TimeUnit>(m.header.timestamp_unit),
        static_cast<protocol::ClockDomain>(m.header.clock_domain),
        m.header.time_mapping_revision, m.header.timestamp_uncertainty);
    std::vector<flatbuffers::Offset<flatbuffers::String>> modules;
    for (const auto& name : m.modules) modules.push_back(b.CreateString(name));
    const auto names = b.CreateVector(modules);
    const auto id = b.CreateString(m.request_id), file = b.CreateString(m.file_name), hash = b.CreateString(m.sha256);
    const auto data = b.CreateVector(m.data);
    const auto code = b.CreateString(m.error_code), detail = b.CreateString(m.detail);
    logwire::MessageBuilder builder(b);
    builder.add_header(header);
    builder.add_version(1);
    builder.add_kind(static_cast<logwire::Kind>(m.kind));
    builder.add_request_id(id);
    builder.add_mode(static_cast<logwire::Mode>(m.mode));
    builder.add_modules(names);
    builder.add_file_name(file);
    builder.add_file_size(m.file_size);
    builder.add_chunk_size(m.chunk_size);
    builder.add_chunk_count(m.chunk_count);
    builder.add_chunk_index(m.chunk_index);
    builder.add_sha256(hash);
    builder.add_data(data);
    builder.add_error_code(code);
    builder.add_detail(detail);
    const auto message = builder.Finish();
    logwire::FinishMessageBuffer(b, message);
    return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}
Message LogCodec::decode(const Bytes& bytes) {
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (!logwire::VerifyMessageBuffer(verifier)) throw Error("BAD_MESSAGE", "invalid FlatBuffer");
    const auto* wire = logwire::GetMessage(bytes.data());
    if (wire->version() != 1 || wire->kind() > logwire::Kind_ERROR || wire->mode() > logwire::Mode_LATEST)
        throw Error("BAD_MESSAGE", "unsupported version or enum");
    Message m;
    const auto* header = wire->header();
    if (!header) throw Error("BAD_MESSAGE", "missing common header");
    m.header.protocol_major = header->protocol_major();
    m.header.protocol_minor = header->protocol_minor();
    if (header->producer_id()) m.header.producer_id = header->producer_id()->str();
    m.header.stream_epoch = header->stream_epoch();
    m.header.sequence = header->sequence();
    m.header.timestamp = header->timestamp();
    m.header.timestamp_unit = static_cast<TimeUnit>(header->timestamp_unit());
    m.header.clock_domain = static_cast<ClockDomain>(header->clock_domain());
    m.header.time_mapping_revision = header->time_mapping_revision();
    m.header.timestamp_uncertainty = header->timestamp_uncertainty();
    if (!m.header.valid()) throw Error("BAD_MESSAGE", "invalid common header");
    m.kind = static_cast<Kind>(wire->kind()); m.mode = static_cast<Mode>(wire->mode());
    m.request_id = wire->request_id()->str();
    if (!valid_segment(m.request_id)) throw Error("BAD_MESSAGE", "invalid request_id");
    if (wire->modules()) {
        if (wire->modules()->size() > 64) throw Error("BAD_MESSAGE", "too many modules");
        for (const auto* name : *wire->modules()) {
            if (!valid_segment(name->str())) throw Error("BAD_MESSAGE", "invalid module name");
            m.modules.push_back(name->str());
        }
    }
    if (wire->file_name()) m.file_name = wire->file_name()->str();
    if (wire->sha256()) m.sha256 = wire->sha256()->str();
    m.file_size = wire->file_size(); m.chunk_size = wire->chunk_size();
    m.chunk_index = wire->chunk_index(); m.chunk_count = wire->chunk_count();
    if (wire->data()) m.data.assign(wire->data()->begin(), wire->data()->end());
    if (wire->error_code()) m.error_code = wire->error_code()->str();
    if (wire->detail()) m.detail = wire->detail()->str();
    return m;
}
}
