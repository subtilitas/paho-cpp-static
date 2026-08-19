// SPDX-License-Identifier: MIT
//
// MQTT 3.1.1 wire vocabulary: packet types, flags, fixed header, and the
// variable byte integer. Pure value types, no I/O, no allocation.

#ifndef MQTT_PACKET_HPP
#define MQTT_PACKET_HPP

#include <cstddef>
#include <cstdint>

#include <etl/span.h>
#include <etl/string_view.h>

#include "mqtt/error.hpp"

namespace mqtt {

/// MQTT control packet types (MQTT 3.1.1 section 2.2.1).
enum class PacketType : uint8_t
{
    Reserved    = 0,
    Connect     = 1,
    Connack     = 2,
    Publish     = 3,
    Puback      = 4,
    Pubrec      = 5,
    Pubrel      = 6,
    Pubcomp     = 7,
    Subscribe   = 8,
    Suback      = 9,
    Unsubscribe = 10,
    Unsuback    = 11,
    Pingreq     = 12,
    Pingresp    = 13,
    Disconnect  = 14,
};

const char* to_string(PacketType t) noexcept;

/// Quality of service level.
enum class QoS : uint8_t
{
    AtMostOnce  = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2,
};

/// CONNACK return codes (MQTT 3.1.1 section 3.2.2.3).
enum class ConnackCode : uint8_t
{
    Accepted                    = 0,
    UnacceptableProtocolVersion = 1,
    IdentifierRejected          = 2,
    ServerUnavailable           = 3,
    BadUsernameOrPassword       = 4,
    NotAuthorized               = 5,
};

const char* to_string(ConnackCode c) noexcept;

/// Marker returned in SUBACK when the broker refuses a filter.
static constexpr uint8_t kSubackFailure = 0x80;

/// Protocol constants.
static constexpr uint8_t  kProtocolLevel_3_1_1 = 4;
static constexpr uint32_t kMaxRemainingLength  = 268435455u;   ///< 256 MB - 1
static constexpr size_t   kMaxVbiBytes         = 4;

//------------------------------------------------------------------------------
// Fixed header
//------------------------------------------------------------------------------

/// The one-byte type/flags header plus the decoded remaining length.
///
/// Unlike Paho's bitfield union, this carries no endianness assumptions and no
/// implementation-defined bitfield layout, so it behaves identically on every
/// target and is safe to memcpy or store.
struct FixedHeader
{
    PacketType type             = PacketType::Reserved;
    bool       dup              = false;
    QoS        qos              = QoS::AtMostOnce;
    bool       retain           = false;
    uint32_t   remaining_length = 0;

    /// Bytes the fixed header itself occupies on the wire.
    size_t header_size() const noexcept;

    /// Total wire size of the packet: fixed header plus remaining length.
    size_t total_size() const noexcept { return header_size() + remaining_length; }

    /// The first byte as it appears on the wire.
    uint8_t to_byte() const noexcept;

    /// Decode the first byte. Validates the flag bits against the packet type,
    /// which catches a large class of malformed or misaligned streams early.
    static Result<FixedHeader> from_byte(uint8_t b) noexcept;
};

//------------------------------------------------------------------------------
// Variable byte integer (remaining length)
//------------------------------------------------------------------------------

/// Number of bytes the variable byte integer encoding of `value` occupies.
/// Returns 0 if the value is out of range.
size_t vbi_size(uint32_t value) noexcept;

/// Encode `value` into `out`. Returns the number of bytes written, or
/// Error::BufferTooSmall / Error::InvalidArgument.
Result<size_t> vbi_encode(etl::span<uint8_t> out, uint32_t value) noexcept;

/// Decoded variable byte integer plus how many bytes it consumed.
struct VbiDecode
{
    uint32_t value = 0;
    size_t   bytes = 0;
};

/// Decode a variable byte integer from the front of `in`.
/// Returns Error::Incomplete if `in` holds a valid but unterminated prefix,
/// and Error::MalformedPacket if the encoding is invalid (more than four bytes).
Result<VbiDecode> vbi_decode(etl::span<const uint8_t> in) noexcept;

//------------------------------------------------------------------------------
// Message and option value types
//------------------------------------------------------------------------------

/// A received message. Topic and payload are *views into the receive buffer*
/// and are only valid for the duration of the callback. Copy anything you need
/// to keep -- into your own storage, since we will not allocate for you.
struct Message
{
    etl::string_view         topic;
    etl::span<const uint8_t> payload;
    QoS                      qos       = QoS::AtMostOnce;
    bool                     retain    = false;
    bool                     dup       = false;
    uint16_t                 packet_id = 0;
};

/// Last will and testament. Views must outlive the connect handshake.
struct Will
{
    etl::string_view         topic;
    etl::span<const uint8_t> payload;
    QoS                      qos    = QoS::AtMostOnce;
    bool                     retain = false;

    bool valid() const noexcept { return !topic.empty(); }
};

/// Parameters for the CONNECT packet.
///
/// The views need only stay valid for the duration of the Client::connect()
/// call: the CONNECT packet is serialized into the transmit queue immediately,
/// so nothing is retained afterwards. Stack-allocated options are fine.
struct ConnectOptions
{
    etl::string_view         client_id;
    etl::string_view         username;   ///< empty = omit
    etl::span<const uint8_t> password;   ///< empty = omit
    Will                     will;       ///< default-constructed = omit
    uint16_t                 keep_alive_s  = 60;
    bool                     clean_session = true;
};

/// Result of a completed CONNECT handshake.
struct ConnackInfo
{
    ConnackCode code            = ConnackCode::Accepted;
    bool        session_present = false;
};

/// One entry of a SUBSCRIBE request.
struct TopicSubscription
{
    etl::string_view filter;
    QoS              qos = QoS::AtMostOnce;
};

}   // namespace mqtt

#endif   // MQTT_PACKET_HPP
