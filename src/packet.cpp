// SPDX-License-Identifier: MIT

#include "mqtt/packet.hpp"

namespace mqtt {

const char* to_string(PacketType t) noexcept
{
    // The returns are aligned into a column on purpose; clang-format has no
    // option that preserves it, so it is switched off across the table.
    // clang-format off
    switch (t)
    {
        case PacketType::Reserved:    return "RESERVED";
        case PacketType::Connect:     return "CONNECT";
        case PacketType::Connack:     return "CONNACK";
        case PacketType::Publish:     return "PUBLISH";
        case PacketType::Puback:      return "PUBACK";
        case PacketType::Pubrec:      return "PUBREC";
        case PacketType::Pubrel:      return "PUBREL";
        case PacketType::Pubcomp:     return "PUBCOMP";
        case PacketType::Subscribe:   return "SUBSCRIBE";
        case PacketType::Suback:      return "SUBACK";
        case PacketType::Unsubscribe: return "UNSUBSCRIBE";
        case PacketType::Unsuback:    return "UNSUBACK";
        case PacketType::Pingreq:     return "PINGREQ";
        case PacketType::Pingresp:    return "PINGRESP";
        case PacketType::Disconnect:  return "DISCONNECT";
    }
    // clang-format on
    return "UNKNOWN";
}

const char* to_string(ConnackCode c) noexcept
{
    // The returns are aligned into a column on purpose; clang-format has no
    // option that preserves it, so it is switched off across the table.
    // clang-format off
    switch (c)
    {
        case ConnackCode::Accepted:                    return "Accepted";
        case ConnackCode::UnacceptableProtocolVersion: return "Unacceptable protocol version";
        case ConnackCode::IdentifierRejected:          return "Identifier rejected";
        case ConnackCode::ServerUnavailable:           return "Server unavailable";
        case ConnackCode::BadUsernameOrPassword:       return "Bad user name or password";
        case ConnackCode::NotAuthorized:               return "Not authorized";
    }
    // clang-format on
    return "Unknown CONNACK code";
}

const char* to_string(Error e) noexcept
{
    // The returns are aligned into a column on purpose; clang-format has no
    // option that preserves it, so it is switched off across the table.
    // clang-format off
    switch (e)
    {
        case Error::Ok:                 return "Ok";
        case Error::WouldBlock:         return "WouldBlock";
        case Error::Incomplete:         return "Incomplete";
        case Error::InvalidArgument:    return "InvalidArgument";
        case Error::NotConnected:       return "NotConnected";
        case Error::AlreadyConnected:   return "AlreadyConnected";
        case Error::NotSupported:       return "NotSupported";
        case Error::BufferTooSmall:     return "BufferTooSmall";
        case Error::TxQueueFull:        return "TxQueueFull";
        case Error::NoInflightSlot:     return "NoInflightSlot";
        case Error::NoInboundSlot:      return "NoInboundSlot";
        case Error::NoSubscriptionSlot: return "NoSubscriptionSlot";
        case Error::NoPendingAckSlot:   return "NoPendingAckSlot";
        case Error::PayloadTooLarge:    return "PayloadTooLarge";
        case Error::TopicTooLong:       return "TopicTooLong";
        case Error::MalformedPacket:    return "MalformedPacket";
        case Error::ProtocolViolation:  return "ProtocolViolation";
        case Error::PacketTooLarge:     return "PacketTooLarge";
        case Error::ConnectionRefused:  return "ConnectionRefused";
        case Error::KeepAliveTimeout:   return "KeepAliveTimeout";
        case Error::ConnectTimeout:     return "ConnectTimeout";
        case Error::TransportFailure:   return "TransportFailure";
        case Error::TransportClosed:    return "TransportClosed";
    }
    // clang-format on
    return "Unknown";
}

//------------------------------------------------------------------------------
// FixedHeader
//------------------------------------------------------------------------------

uint8_t FixedHeader::to_byte() const noexcept
{
    uint8_t b = static_cast<uint8_t>(static_cast<uint8_t>(type) << 4);

    if (type == PacketType::Publish)
    {
        // The flag bits line up with the diagram in section 2.2.2; keeping them
        // one per line and aligned is the point.
        // clang-format off
        if (dup)    b |= 0x08;
        b |= static_cast<uint8_t>((static_cast<uint8_t>(qos) & 0x03) << 1);
        if (retain) b |= 0x01;
        // clang-format on
    }
    else if (type == PacketType::Pubrel || type == PacketType::Subscribe ||
             type == PacketType::Unsubscribe)
    {
        // Spec-mandated fixed flags 0b0010 for these three.
        b |= 0x02;
    }

    return b;
}

Result<FixedHeader> FixedHeader::from_byte(uint8_t b) noexcept
{
    FixedHeader   h;
    const uint8_t type_nibble = static_cast<uint8_t>((b >> 4) & 0x0F);
    const uint8_t flags       = static_cast<uint8_t>(b & 0x0F);

    if (type_nibble == 0 || type_nibble > static_cast<uint8_t>(PacketType::Disconnect))
        return Error::MalformedPacket;

    h.type = static_cast<PacketType>(type_nibble);

    if (h.type == PacketType::Publish)
    {
        h.dup           = (flags & 0x08) != 0;
        const uint8_t q = static_cast<uint8_t>((flags >> 1) & 0x03);
        if (q > 2)
            return Error::MalformedPacket;   // QoS 3 is a protocol error
        h.qos    = static_cast<QoS>(q);
        h.retain = (flags & 0x01) != 0;
    }
    else
    {
        // Every other packet type has mandated flag bits; a mismatch means we
        // are misaligned in the stream or the peer is broken.
        const uint8_t expected =
            (h.type == PacketType::Pubrel || h.type == PacketType::Subscribe ||
             h.type == PacketType::Unsubscribe)
                ? 0x02u
                : 0x00u;
        if (flags != expected)
            return Error::MalformedPacket;
    }

    return h;
}

size_t FixedHeader::header_size() const noexcept
{
    const size_t n = vbi_size(remaining_length);
    return (n == 0) ? 0 : (1u + n);
}

//------------------------------------------------------------------------------
// Variable byte integer
//------------------------------------------------------------------------------

size_t vbi_size(uint32_t value) noexcept
{
    if (value > kMaxRemainingLength)
        return 0;
    if (value < 128u)
        return 1;
    if (value < 16384u)
        return 2;
    if (value < 2097152u)
        return 3;
    return 4;
}

Result<size_t> vbi_encode(etl::span<uint8_t> out, uint32_t value) noexcept
{
    if (value > kMaxRemainingLength)
        return Error::InvalidArgument;

    const size_t needed = vbi_size(value);
    if (out.size() < needed)
        return Error::BufferTooSmall;

    size_t i = 0;
    do
    {
        uint8_t byte = static_cast<uint8_t>(value % 128u);
        value /= 128u;
        if (value > 0)
            byte = static_cast<uint8_t>(byte | 0x80u);
        out[i++] = byte;
    } while (value > 0);

    return i;
}

Result<VbiDecode> vbi_decode(etl::span<const uint8_t> in) noexcept
{
    VbiDecode d;
    uint32_t  multiplier = 1;

    for (size_t i = 0; i < kMaxVbiBytes; ++i)
    {
        if (i >= in.size())
            return Error::Incomplete;   // valid prefix, need more bytes

        const uint8_t byte = in[i];
        d.value += static_cast<uint32_t>(byte & 0x7Fu) * multiplier;

        if ((byte & 0x80u) == 0)
        {
            d.bytes = i + 1;
            // Reject non-minimal encodings such as 0x80 0x00, which some
            // fuzzers and broken brokers emit and which Paho accepts silently.
            if (d.bytes != vbi_size(d.value))
                return Error::MalformedPacket;
            return d;
        }

        multiplier *= 128u;
    }

    return Error::MalformedPacket;   // fifth continuation byte
}

}   // namespace mqtt
