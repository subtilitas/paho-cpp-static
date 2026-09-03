// Fuzzes the decoders directly, on arbitrary bytes.
//
// fuzz_client covers the same code through the state machine, which is the
// realistic path. This one reaches it without that filter: the client only
// calls a decoder once peek_header has accepted the fixed header and the
// packet is complete in the buffer, so decoder behaviour on input the client
// would never hand it is untested there by construction.
//
// It exists because those preconditions are an argument, not a guarantee, and
// a decoder that is only safe because of its caller is one refactor away from
// not being safe.
//
// Build:  cmake -S . -B build-fuzz -DMQTT_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
// Run:    ./build-fuzz/tests/fuzz_codec -max_total_time=60 tests/fuzz/corpus/codec
//
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "mqtt/codec.hpp"
#include "mqtt/packet.hpp"
#include "mqtt/topic.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    const etl::span<const uint8_t> in(data, size);

    // The fixed header parser sees the raw stream first, so it gets the whole
    // input including the lengths it is supposed to distrust.
    const mqtt::Result<mqtt::codec::PacketPeek> peek = mqtt::codec::peek_header(in);

    // The body decoders take what follows the fixed header. Feed them the tail
    // when the peek succeeded, and the whole input when it did not -- the
    // second is the case the client never produces.
    const size_t offset =
        peek.ok() ? (peek.value().header_bytes < size ? peek.value().header_bytes : size) : 0;

    const etl::span<const uint8_t> body(data + offset, size - offset);

    (void)mqtt::codec::decode_connack(body);
    (void)mqtt::codec::decode_packet_id(body);
    (void)mqtt::codec::decode_suback(body);

    if (peek.ok())
    {
        (void)mqtt::codec::decode_publish(peek.value().header, body);
    }
    else
    {
        // A synthesised header, so decode_publish is reached even when the
        // peek rejected the input. The QoS and flags come from the data.
        mqtt::FixedHeader header{};
        header.type   = mqtt::PacketType::Publish;
        header.qos    = static_cast<mqtt::QoS>(data[0] & 0x03u);
        header.dup    = (data[0] & 0x08u) != 0;
        header.retain = (data[0] & 0x10u) != 0;

        (void)mqtt::codec::decode_publish(header, body);
    }

    // Topic matching runs on peer-supplied bytes too: a PUBLISH topic is
    // matched against every stored filter, so both sides come off the wire.
    if (size >= 2)
    {
        const size_t split = static_cast<size_t>(data[0]) % size;

        const etl::string_view filter(reinterpret_cast<const char*>(data), split);
        const etl::string_view topic(reinterpret_cast<const char*>(data + split), size - split);

        (void)mqtt::topic_matches(filter, topic);
        (void)mqtt::is_valid_filter(filter);
        (void)mqtt::is_valid_topic_name(topic);
    }

    return 0;
}
