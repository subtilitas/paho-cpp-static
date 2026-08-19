// SPDX-License-Identifier: MIT
//
// MQTT 3.1.1 packet serialization, built on etl::byte_stream_writer /
// etl::byte_stream_reader.
//
// Every function here is a pure transformation between a caller-owned byte span
// and a value type. Nothing allocates, nothing is stateful, and every bounds
// check is explicit -- the ETL streams return bool/optional on overrun rather
// than throwing, which is why they are a good fit for a -fno-exceptions build.
//
// Decoded views (topic, payload, granted-QoS list) point *into the caller's
// input span*. They are valid exactly as long as that span is.

#ifndef MQTT_CODEC_HPP
#define MQTT_CODEC_HPP

#include <etl/byte_stream.h>
#include <etl/span.h>
#include <etl/string_view.h>

#include "mqtt/error.hpp"
#include "mqtt/packet.hpp"

namespace mqtt {
namespace codec {

/// Byte order used for every multi-byte field in MQTT.
static constexpr etl::endian kNetworkOrder = etl::endian::big;

//------------------------------------------------------------------------------
// Size computation
//
// Sizes are computed up front so the fixed header (which embeds the remaining
// length) can be written in one pass, with no back-patching and no scratch copy.
//------------------------------------------------------------------------------

/// Remaining length of the CONNECT packet these options produce.
Result<uint32_t> connect_remaining_length(const ConnectOptions& opts) noexcept;

/// Remaining length of a PUBLISH with this topic, payload and QoS.
Result<uint32_t> publish_remaining_length(etl::string_view topic, size_t payload_len,
                                          QoS qos) noexcept;

/// Remaining length of a SUBSCRIBE covering these subscriptions.
Result<uint32_t> subscribe_remaining_length(etl::span<const TopicSubscription> subs) noexcept;

/// Remaining length of an UNSUBSCRIBE covering these filters.
Result<uint32_t>
unsubscribe_remaining_length(etl::span<const etl::string_view> filters) noexcept;

//------------------------------------------------------------------------------
// Encoding
//
// Each encode_* writes a complete packet (fixed header included) into `out` and
// returns the number of bytes written, or an error if `out` was too small.
//------------------------------------------------------------------------------

Result<size_t> encode_connect(etl::span<uint8_t> out, const ConnectOptions& opts) noexcept;

Result<size_t> encode_publish(etl::span<uint8_t> out, etl::string_view topic,
                              etl::span<const uint8_t> payload, QoS qos, bool retain, bool dup,
                              uint16_t packet_id) noexcept;

/// PUBACK, PUBREC, PUBREL or PUBCOMP -- all share the same two-byte body.
Result<size_t> encode_ack(etl::span<uint8_t> out, PacketType type, uint16_t packet_id) noexcept;

Result<size_t> encode_subscribe(etl::span<uint8_t> out, uint16_t packet_id,
                                etl::span<const TopicSubscription> subs) noexcept;

Result<size_t> encode_unsubscribe(etl::span<uint8_t> out, uint16_t packet_id,
                                  etl::span<const etl::string_view> filters) noexcept;

/// PINGREQ or DISCONNECT -- header only, no body.
Result<size_t> encode_empty(etl::span<uint8_t> out, PacketType type) noexcept;

//------------------------------------------------------------------------------
// Decoding
//------------------------------------------------------------------------------

/// Peek at the front of a receive buffer to work out how big the pending packet
/// is, without consuming anything.
///
/// Returns Error::Incomplete while the fixed header is still arriving, which is
/// the normal case on a stream transport and not a failure.
struct PacketPeek
{
    FixedHeader header;
    size_t      header_bytes = 0;   ///< size of the fixed header itself
    size_t      total_bytes  = 0;   ///< header_bytes + header.remaining_length
};

Result<PacketPeek> peek_header(etl::span<const uint8_t> in) noexcept;

/// Decode CONNACK. `body` is the remaining-length region, fixed header removed.
Result<ConnackInfo> decode_connack(etl::span<const uint8_t> body) noexcept;

/// Decode PUBLISH. Topic and payload in the result view into `body`.
Result<Message> decode_publish(const FixedHeader&       header,
                               etl::span<const uint8_t> body) noexcept;

/// Decode PUBACK / PUBREC / PUBREL / PUBCOMP / UNSUBACK: a bare packet id.
Result<uint16_t> decode_packet_id(etl::span<const uint8_t> body) noexcept;

/// Decode SUBACK. `return_codes` views into `body`; each entry is a granted QoS
/// (0..2) or kSubackFailure (0x80).
struct SubackView
{
    uint16_t                 packet_id = 0;
    etl::span<const uint8_t> return_codes;
};

Result<SubackView> decode_suback(etl::span<const uint8_t> body) noexcept;

//------------------------------------------------------------------------------
// Field-level helpers, exposed for tests and for callers extending the codec
//------------------------------------------------------------------------------

/// Wire size of a length-prefixed UTF-8 string: two length bytes plus the data.
constexpr size_t utf8_size(size_t len) noexcept { return 2u + len; }

/// Write a length-prefixed UTF-8 string.
Error write_string(etl::byte_stream_writer& w, etl::string_view s) noexcept;

/// Write length-prefixed binary data (same wire format, different intent).
Error write_binary(etl::byte_stream_writer& w, etl::span<const uint8_t> d) noexcept;

/// Read a length-prefixed UTF-8 string as a view into the stream's buffer.
Result<etl::string_view> read_string(etl::byte_stream_reader& r) noexcept;

}   // namespace codec
}   // namespace mqtt

#endif   // MQTT_CODEC_HPP
