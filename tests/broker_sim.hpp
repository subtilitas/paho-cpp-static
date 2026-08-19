// Helpers for driving the client from the broker side of the fake transport,
// and for inspecting what the client wrote.

#ifndef MQTT_TEST_BROKER_SIM_HPP
#define MQTT_TEST_BROKER_SIM_HPP

#include <cstring>

#include "fakes.hpp"
#include "mqtt/codec.hpp"

namespace sim {

using namespace mqtt;

//------------------------------------------------------------------------------
// Broker -> client
//------------------------------------------------------------------------------

inline void push_connack(fakes::FakeTransport& t, bool session_present,
                         ConnackCode code = ConnackCode::Accepted) noexcept
{
    const uint8_t bytes[] = {0x20, 0x02, static_cast<uint8_t>(session_present ? 1 : 0),
                             static_cast<uint8_t>(code)};
    t.push_inbound(bytes, sizeof(bytes));
}

inline void push_ack(fakes::FakeTransport& t, PacketType type, uint16_t id) noexcept
{
    uint8_t              buf[8] = {};
    const Result<size_t> n = codec::encode_ack(etl::span<uint8_t>(buf, sizeof(buf)), type, id);
    if (n.ok())
        t.push_inbound(buf, n.value());
}

inline void push_pingresp(fakes::FakeTransport& t) noexcept
{
    const uint8_t bytes[] = {0xD0, 0x00};
    t.push_inbound(bytes, sizeof(bytes));
}

inline void push_suback(fakes::FakeTransport& t, uint16_t id, const uint8_t* codes,
                        size_t count) noexcept
{
    uint8_t buf[32];
    buf[0] = 0x90;
    buf[1] = static_cast<uint8_t>(2 + count);
    buf[2] = static_cast<uint8_t>(id >> 8);
    buf[3] = static_cast<uint8_t>(id & 0xFF);
    std::memcpy(buf + 4, codes, count);
    t.push_inbound(buf, 4 + count);
}

inline void push_unsuback(fakes::FakeTransport& t, uint16_t id) noexcept
{
    const uint8_t bytes[] = {0xB0, 0x02, static_cast<uint8_t>(id >> 8),
                             static_cast<uint8_t>(id & 0xFF)};
    t.push_inbound(bytes, sizeof(bytes));
}

/// Build a PUBLISH as the broker would send it.
inline void push_publish(fakes::FakeTransport& t, const char* topic, const char* payload,
                         QoS qos = QoS::AtMostOnce, uint16_t id = 0, bool dup = false,
                         bool retain = false) noexcept
{
    uint8_t              buf[512]    = {};
    const size_t         payload_len = std::strlen(payload);
    const Result<size_t> n           = codec::encode_publish(
        etl::span<uint8_t>(buf, sizeof(buf)), etl::string_view(topic),
        etl::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload), payload_len), qos,
        retain, dup, id);
    if (n.ok())
        t.push_inbound(buf, n.value());
}

//------------------------------------------------------------------------------
// Client -> broker inspection
//------------------------------------------------------------------------------

struct SentPacket
{
    FixedHeader    header;
    const uint8_t* body     = nullptr;
    size_t         body_len = 0;
    bool           valid    = false;
};

/// Walk the byte stream the client produced and return the `index`th packet of
/// `type`, or an invalid packet if there is no such thing.
inline SentPacket find_sent(const fakes::FakeTransport& t, PacketType type,
                            size_t index = 0) noexcept
{
    size_t pos     = 0;
    size_t matched = 0;

    while (pos < t.sent.size())
    {
        const Result<codec::PacketPeek> peek = codec::peek_header(
            etl::span<const uint8_t>(t.sent.data() + pos, t.sent.size() - pos));
        if (!peek.ok())
            break;

        const codec::PacketPeek& p = peek.value();
        if (pos + p.total_bytes > t.sent.size())
            break;

        if (p.header.type == type)
        {
            if (matched == index)
            {
                SentPacket sp;
                sp.header   = p.header;
                sp.body     = t.sent.data() + pos + p.header_bytes;
                sp.body_len = p.header.remaining_length;
                sp.valid    = true;
                return sp;
            }
            ++matched;
        }

        pos += p.total_bytes;
    }

    return SentPacket{};
}

/// How many packets of `type` the client has written.
inline size_t count_sent(const fakes::FakeTransport& t, PacketType type) noexcept
{
    size_t pos = 0;
    size_t n   = 0;

    while (pos < t.sent.size())
    {
        const Result<codec::PacketPeek> peek = codec::peek_header(
            etl::span<const uint8_t>(t.sent.data() + pos, t.sent.size() - pos));
        if (!peek.ok())
            break;
        if (pos + peek.value().total_bytes > t.sent.size())
            break;
        if (peek.value().header.type == type)
            ++n;
        pos += peek.value().total_bytes;
    }
    return n;
}

/// The packet id carried by a two-byte-body ack the client sent.
inline uint16_t sent_ack_id(const SentPacket& p) noexcept
{
    if (!p.valid || p.body_len < 2)
        return 0;
    return static_cast<uint16_t>((static_cast<uint16_t>(p.body[0]) << 8) | p.body[1]);
}

}   // namespace sim

#endif   // MQTT_TEST_BROKER_SIM_HPP
