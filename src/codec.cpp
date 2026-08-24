// SPDX-License-Identifier: MIT

#include "mqtt/codec.hpp"

#include <etl/byte_stream.h>

#include "mqtt/utf8.hpp"

namespace mqtt {
namespace codec {
namespace {

/// Largest value that can be length-prefixed by a two-byte MQTT string header.
constexpr size_t kMaxStringLen = 65535u;

/// Add `add` to `total`, failing if the result would exceed the MQTT maximum.
/// Doing the arithmetic through this helper keeps every length computation
/// overflow-safe on 16-bit size_t targets as well as 32-bit ones.
bool checked_add(uint32_t& total, uint64_t add) noexcept
{
    const uint64_t sum = static_cast<uint64_t>(total) + add;
    if (sum > kMaxRemainingLength)
        return false;
    total = static_cast<uint32_t>(sum);
    return true;
}

/// QoS is an enum class, which constrains what callers *mean* but not what
/// they can produce: a value cast from a config file or a wire byte can hold 3,
/// which the spec forbids everywhere it appears. Encoding is the last place to
/// catch it, so check rather than truncate -- a silently masked QoS would put a
/// packet on the wire that this library's own decoder rejects.
bool valid_qos(QoS q) noexcept { return static_cast<uint8_t>(q) <= 2u; }

/// Write the fixed header for `type` with the given flags and remaining length.
Error write_fixed_header(etl::byte_stream_writer& w, PacketType type, uint32_t remaining_length,
                         bool dup = false, QoS qos = QoS::AtMostOnce,
                         bool retain = false) noexcept
{
    FixedHeader h;
    h.type             = type;
    h.dup              = dup;
    h.qos              = qos;
    h.retain           = retain;
    h.remaining_length = remaining_length;

    MQTT_WRITE(w.write<uint8_t>(h.to_byte()));

    uint8_t              vbi[kMaxVbiBytes];
    const Result<size_t> n =
        vbi_encode(etl::span<uint8_t>(vbi, kMaxVbiBytes), remaining_length);
    if (!n.ok())
        return n.error();

    MQTT_WRITE(w.write<uint8_t>(vbi, n.value()));
    return Error::Ok;
}

}   // namespace

//------------------------------------------------------------------------------
// Field helpers
//------------------------------------------------------------------------------

Error write_string(etl::byte_stream_writer& w, etl::string_view s) noexcept
{
    if (s.size() > kMaxStringLen)
        return Error::InvalidArgument;

    MQTT_WRITE(w.write<uint16_t>(static_cast<uint16_t>(s.size())));
    if (!s.empty())
        MQTT_WRITE(w.write<uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
    return Error::Ok;
}

Error write_binary(etl::byte_stream_writer& w, etl::span<const uint8_t> d) noexcept
{
    if (d.size() > kMaxStringLen)
        return Error::InvalidArgument;

    MQTT_WRITE(w.write<uint16_t>(static_cast<uint16_t>(d.size())));
    if (!d.empty())
        MQTT_WRITE(w.write<uint8_t>(d.data(), d.size()));
    return Error::Ok;
}

Result<etl::string_view> read_string(etl::byte_stream_reader& r) noexcept
{
    uint16_t len = 0;
    {
        auto opt = r.read<uint16_t>();
        if (!opt.has_value())
            return Error::MalformedPacket;
        len = opt.value();
    }

    if (len == 0)
        return etl::string_view{};

    auto data = r.read<char>(len);
    if (!data.has_value())
        return Error::MalformedPacket;

    const etl::string_view s(data.value().data(), len);

    // MQTT-1.5.3-1: a receiver that finds a malformed UTF-8 string must close
    // the connection, which is what MalformedPacket does. This is the check
    // that keeps the application from being handed bytes it will treat as
    // text -- everything else here is about not *emitting* nonsense.
    if (!is_valid_mqtt_string(s))
        return Error::MalformedPacket;

    return s;
}

//------------------------------------------------------------------------------
// Size computation
//------------------------------------------------------------------------------

Result<uint32_t> connect_remaining_length(const ConnectOptions& opts) noexcept
{
    if (opts.client_id.size() > kMaxStringLen)
        return Error::InvalidArgument;
    if (!is_valid_mqtt_string(opts.client_id))
        return Error::InvalidArgument;   // MQTT-1.5.3-1

    // Variable header: protocol name ("MQTT") + level + flags + keep alive.
    uint32_t total = 0;
    if (!checked_add(total, utf8_size(4) + 1u + 1u + 2u))
        return Error::InvalidArgument;

    // Payload, in the order mandated by the spec.
    if (!checked_add(total, utf8_size(opts.client_id.size())))
        return Error::InvalidArgument;

    if (opts.will.valid())
    {
        if (!valid_qos(opts.will.qos))
            return Error::InvalidArgument;   // MQTT-3.1.2-14
        if (opts.will.topic.size() > kMaxStringLen || opts.will.payload.size() > kMaxStringLen)
            return Error::InvalidArgument;
        // The will *topic* is a UTF-8 string; the will *payload* is arbitrary
        // bytes and is deliberately not checked.
        if (!is_valid_mqtt_string(opts.will.topic))
            return Error::InvalidArgument;
        if (!checked_add(total, utf8_size(opts.will.topic.size())) ||
            !checked_add(total, utf8_size(opts.will.payload.size())))
            return Error::InvalidArgument;
    }

    if (!opts.username.empty())
    {
        if (opts.username.size() > kMaxStringLen)
            return Error::InvalidArgument;
        // The password is a binary field per the spec, so only the username
        // is checked here.
        if (!is_valid_mqtt_string(opts.username))
            return Error::InvalidArgument;
        if (!checked_add(total, utf8_size(opts.username.size())))
            return Error::InvalidArgument;
    }

    if (!opts.password.empty())
    {
        // The spec forbids a password without a username.
        if (opts.username.empty())
            return Error::InvalidArgument;
        if (opts.password.size() > kMaxStringLen)
            return Error::InvalidArgument;
        if (!checked_add(total, utf8_size(opts.password.size())))
            return Error::InvalidArgument;
    }

    return total;
}

Result<uint32_t> publish_remaining_length(etl::string_view topic, size_t payload_len,
                                          QoS qos) noexcept
{
    if (topic.empty() || topic.size() > kMaxStringLen)
        return Error::InvalidArgument;
    if (!valid_qos(qos))
        return Error::InvalidArgument;

    if (!is_valid_mqtt_string(topic))
        return Error::InvalidArgument;   // MQTT-1.5.3-1

    uint32_t total = 0;
    if (!checked_add(total, utf8_size(topic.size())))
        return Error::InvalidArgument;

    if (qos != QoS::AtMostOnce)
    {
        if (!checked_add(total, 2u))
            return Error::InvalidArgument;
    }

    if (!checked_add(total, payload_len))
        return Error::PayloadTooLarge;

    return total;
}

Result<uint32_t> subscribe_remaining_length(etl::span<const TopicSubscription> subs) noexcept
{
    if (subs.empty())
        return Error::InvalidArgument;   // SUBSCRIBE must carry at least one filter

    uint32_t total = 2;   // packet id
    for (const TopicSubscription& s : subs)
    {
        if (s.filter.empty() || s.filter.size() > kMaxStringLen)
            return Error::InvalidArgument;
        if (!is_valid_mqtt_string(s.filter))
            return Error::InvalidArgument;
        if (!valid_qos(s.qos))
            return Error::InvalidArgument;                          // MQTT-3-8.3-4
        if (!checked_add(total, utf8_size(s.filter.size()) + 1u))   // +1 requested QoS
            return Error::InvalidArgument;
    }
    return total;
}

Result<uint32_t>
unsubscribe_remaining_length(etl::span<const etl::string_view> filters) noexcept
{
    if (filters.empty())
        return Error::InvalidArgument;

    uint32_t total = 2;   // packet id
    for (const etl::string_view& f : filters)
    {
        if (f.empty() || f.size() > kMaxStringLen)
            return Error::InvalidArgument;
        if (!is_valid_mqtt_string(f))
            return Error::InvalidArgument;
        if (!checked_add(total, utf8_size(f.size())))
            return Error::InvalidArgument;
    }
    return total;
}

//------------------------------------------------------------------------------
// Encoding
//------------------------------------------------------------------------------

Result<size_t> encode_connect(etl::span<uint8_t> out, const ConnectOptions& opts) noexcept
{
    const Result<uint32_t> rl = connect_remaining_length(opts);
    if (!rl.ok())
        return rl.error();

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, PacketType::Connect, rl.value());
    if (hdr != Error::Ok)
        return hdr;

    // Variable header.
    const Error name = write_string(w, etl::string_view("MQTT", 4));
    if (name != Error::Ok)
        return name;

    if (!w.write<uint8_t>(kProtocolLevel_3_1_1))
        return Error::BufferTooSmall;

    uint8_t flags = 0;
    if (opts.clean_session)
        flags |= 0x02;
    if (opts.will.valid())
    {
        flags |= 0x04;
        flags =
            static_cast<uint8_t>(flags | ((static_cast<uint8_t>(opts.will.qos) & 0x03u) << 3));
        if (opts.will.retain)
            flags |= 0x20;
    }
    if (!opts.password.empty())
        flags |= 0x40;
    if (!opts.username.empty())
        flags |= 0x80;

    if (!w.write<uint8_t>(flags))
        return Error::BufferTooSmall;
    if (!w.write<uint16_t>(opts.keep_alive_s))
        return Error::BufferTooSmall;

    // Payload, in spec order: client id, will topic, will message, user, pass.
    {
        const Error e = write_string(w, opts.client_id);
        if (e != Error::Ok)
            return e;
    }
    if (opts.will.valid())
    {
        const Error e1 = write_string(w, opts.will.topic);
        if (e1 != Error::Ok)
            return e1;
        const Error e2 = write_binary(w, opts.will.payload);
        if (e2 != Error::Ok)
            return e2;
    }
    if (!opts.username.empty())
    {
        const Error e = write_string(w, opts.username);
        if (e != Error::Ok)
            return e;
    }
    if (!opts.password.empty())
    {
        const Error e = write_binary(w, opts.password);
        if (e != Error::Ok)
            return e;
    }

    return w.size_bytes();
}

Result<size_t> encode_publish(etl::span<uint8_t> out, etl::string_view topic,
                              etl::span<const uint8_t> payload, QoS qos, bool retain, bool dup,
                              uint16_t packet_id) noexcept
{
    if (qos != QoS::AtMostOnce && packet_id == 0)
        return Error::InvalidArgument;   // QoS > 0 requires a non-zero packet id
    if (qos == QoS::AtMostOnce && dup)
        return Error::InvalidArgument;   // MQTT-3.3.1-2: DUP must be 0 at QoS 0

    const Result<uint32_t> rl = publish_remaining_length(topic, payload.size(), qos);
    if (!rl.ok())
        return rl.error();

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, PacketType::Publish, rl.value(), dup, qos, retain);
    if (hdr != Error::Ok)
        return hdr;

    const Error t = write_string(w, topic);
    if (t != Error::Ok)
        return t;

    if (qos != QoS::AtMostOnce)
    {
        if (!w.write<uint16_t>(packet_id))
            return Error::BufferTooSmall;
    }

    if (!payload.empty())
    {
        if (!w.write<uint8_t>(payload.data(), payload.size()))
            return Error::BufferTooSmall;
    }

    return w.size_bytes();
}

Result<size_t> encode_ack(etl::span<uint8_t> out, PacketType type, uint16_t packet_id) noexcept
{
    switch (type)
    {
        case PacketType::Puback:
        case PacketType::Pubrec:
        case PacketType::Pubrel:
        case PacketType::Pubcomp: break;
        default: return Error::InvalidArgument;
    }

    if (packet_id == 0)
        return Error::InvalidArgument;

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, type, 2u);
    if (hdr != Error::Ok)
        return hdr;

    if (!w.write<uint16_t>(packet_id))
        return Error::BufferTooSmall;

    return w.size_bytes();
}

Result<size_t> encode_subscribe(etl::span<uint8_t> out, uint16_t packet_id,
                                etl::span<const TopicSubscription> subs) noexcept
{
    if (packet_id == 0)
        return Error::InvalidArgument;

    const Result<uint32_t> rl = subscribe_remaining_length(subs);
    if (!rl.ok())
        return rl.error();

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, PacketType::Subscribe, rl.value());
    if (hdr != Error::Ok)
        return hdr;

    if (!w.write<uint16_t>(packet_id))
        return Error::BufferTooSmall;

    for (const TopicSubscription& s : subs)
    {
        const Error e = write_string(w, s.filter);
        if (e != Error::Ok)
            return e;
        if (!w.write<uint8_t>(static_cast<uint8_t>(s.qos)))
            return Error::BufferTooSmall;
    }

    return w.size_bytes();
}

Result<size_t> encode_unsubscribe(etl::span<uint8_t> out, uint16_t packet_id,
                                  etl::span<const etl::string_view> filters) noexcept
{
    if (packet_id == 0)
        return Error::InvalidArgument;

    const Result<uint32_t> rl = unsubscribe_remaining_length(filters);
    if (!rl.ok())
        return rl.error();

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, PacketType::Unsubscribe, rl.value());
    if (hdr != Error::Ok)
        return hdr;

    if (!w.write<uint16_t>(packet_id))
        return Error::BufferTooSmall;

    for (const etl::string_view& f : filters)
    {
        const Error e = write_string(w, f);
        if (e != Error::Ok)
            return e;
    }

    return w.size_bytes();
}

Result<size_t> encode_empty(etl::span<uint8_t> out, PacketType type) noexcept
{
    if (type != PacketType::Pingreq && type != PacketType::Disconnect)
        return Error::InvalidArgument;

    etl::byte_stream_writer w(out.data(), out.size(), kNetworkOrder);

    const Error hdr = write_fixed_header(w, type, 0u);
    if (hdr != Error::Ok)
        return hdr;

    return w.size_bytes();
}

//------------------------------------------------------------------------------
// Decoding
//------------------------------------------------------------------------------

Result<PacketPeek> peek_header(etl::span<const uint8_t> in) noexcept
{
    if (in.empty())
        return Error::Incomplete;

    const Result<FixedHeader> h = FixedHeader::from_byte(in[0]);
    if (!h.ok())
        return h.error();

    const Result<VbiDecode> vbi = vbi_decode(in.subspan(1));
    if (!vbi.ok())
        return vbi.error();

    PacketPeek peek;
    peek.header                  = h.value();
    peek.header.remaining_length = vbi.value().value;
    peek.header_bytes            = 1u + vbi.value().bytes;
    peek.total_bytes             = peek.header_bytes + vbi.value().value;

    return peek;
}

Result<ConnackInfo> decode_connack(etl::span<const uint8_t> body) noexcept
{
    if (body.size() != 2)
        return Error::MalformedPacket;

    // Bits 7..1 of the acknowledge flags byte are reserved and must be zero.
    if ((body[0] & 0xFEu) != 0)
        return Error::ProtocolViolation;

    if (body[1] > static_cast<uint8_t>(ConnackCode::NotAuthorized))
        return Error::ProtocolViolation;

    // MQTT-3.2.2-4: a refusal must carry session present = 0. A broker that
    // sets both is telling us it resumed a session it also declined to open.
    if ((body[0] & 0x01u) != 0 && body[1] != 0)
        return Error::ProtocolViolation;

    ConnackInfo info;
    info.session_present = (body[0] & 0x01u) != 0;
    info.code            = static_cast<ConnackCode>(body[1]);
    return info;
}

Result<Message> decode_publish(const FixedHeader&       header,
                               etl::span<const uint8_t> body) noexcept
{
    etl::byte_stream_reader r(body.data(), body.size(), kNetworkOrder);

    const Result<etl::string_view> topic = read_string(r);
    if (!topic.ok())
        return topic.error();
    if (topic.value().empty())
        return Error::ProtocolViolation;   // PUBLISH topic must be non-empty

    Message msg;
    msg.topic  = topic.value();
    msg.qos    = header.qos;
    msg.retain = header.retain;
    msg.dup    = header.dup;

    if (header.qos != QoS::AtMostOnce)
    {
        auto id = r.read<uint16_t>();
        if (!id.has_value())
            return Error::MalformedPacket;
        if (id.value() == 0)
            return Error::ProtocolViolation;
        msg.packet_id = id.value();
    }
    else if (header.dup)
    {
        // DUP must be 0 for QoS 0 (MQTT 3.1.1 section 3.3.1.1).
        return Error::ProtocolViolation;
    }

    // Note: byte_stream_reader::size_bytes() reports the stream's total length,
    // not how much has been consumed (the writer's identically-named method
    // means the opposite). Derive the offset from what is left instead.
    const size_t consumed = body.size() - r.available_bytes();
    msg.payload           = body.subspan(consumed);

    return msg;
}

Result<uint16_t> decode_packet_id(etl::span<const uint8_t> body) noexcept
{
    if (body.size() != 2)
        return Error::MalformedPacket;

    const uint16_t id = static_cast<uint16_t>((static_cast<uint16_t>(body[0]) << 8) | body[1]);
    if (id == 0)
        return Error::ProtocolViolation;

    return id;
}

Result<SubackView> decode_suback(etl::span<const uint8_t> body) noexcept
{
    if (body.size() < 3)
        return Error::MalformedPacket;   // packet id plus at least one return code

    const uint16_t id = static_cast<uint16_t>((static_cast<uint16_t>(body[0]) << 8) | body[1]);
    if (id == 0)
        return Error::ProtocolViolation;

    const etl::span<const uint8_t> codes = body.subspan(2);
    for (const uint8_t c : codes)
    {
        if (c > 2u && c != kSubackFailure)
            return Error::ProtocolViolation;
    }

    SubackView v;
    v.packet_id    = id;
    v.return_codes = codes;
    return v;
}

}   // namespace codec
}   // namespace mqtt
