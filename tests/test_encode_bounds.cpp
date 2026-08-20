// Every encoder writes into a caller-supplied span, and every one of them has
// a chain of "if this write did not fit, give up" returns that the rest of the
// suite never reaches, because it always passes a buffer that is plainly big
// enough. Those returns are the difference between refusing to serialize and
// running off the end of somebody's stack buffer.
//
// So rather than pick a size and hope, each encoder is run into every size from
// zero up to a little past what it needs, with guard bytes on both sides. Three
// things must hold at every size:
//
//   * the call either succeeds or reports a bounded-buffer error -- never
//     anything else, and never a crash;
//   * on success it reports no more bytes than the buffer held, and produces a
//     byte-for-byte prefix of what an ample buffer produces;
//   * the guards are untouched, whether it succeeded or not.
//
// The last one is the point. A partial write that overruns by one byte would
// pass a "did it return an error" test and fail this.

#include "test_harness.hpp"

#include <cstring>

#include "mqtt/codec.hpp"

using namespace mqtt;

namespace {

constexpr uint8_t kGuard    = 0xA5;
constexpr size_t  kGuardLen = 8;
constexpr size_t  kMaxProbe = 96;

bool bounded_buffer_error(Error e) noexcept
{
    return e == Error::BufferTooSmall || e == Error::InvalidArgument;
}

/// Run `encode` into every buffer size in [0, kMaxProbe], checking the three
/// invariants above. `name` only appears when something fails.
template <typename EncodeFn>
void sweep(const char* name, EncodeFn&& encode) noexcept
{
    // What an ample buffer produces, to compare prefixes against.
    uint8_t              reference[kMaxProbe] = {};
    const Result<size_t> full = encode(etl::span<uint8_t>(reference, sizeof(reference)));
    if (!CHECK(full.ok()))
    {
        std::printf("    %s: could not encode even into %zu bytes\n", name, kMaxProbe);
        return;
    }

    for (size_t size = 0; size <= kMaxProbe; ++size)
    {
        uint8_t arena[kGuardLen + kMaxProbe + kGuardLen];
        std::memset(arena, kGuard, sizeof(arena));
        uint8_t* buf = arena + kGuardLen;

        const Result<size_t> r = encode(etl::span<uint8_t>(buf, size));

        // Guards first: an overrun matters even when the call reported failure.
        for (size_t i = 0; i < kGuardLen; ++i)
        {
            if (arena[i] != kGuard || arena[kGuardLen + kMaxProbe + i] != kGuard)
            {
                th::report_failure("encoder wrote outside its buffer", __FILE__, __LINE__);
                std::printf("    %s at size %zu\n", name, size);
                return;
            }
        }
        // Nothing may be written past the size it was given, either.
        for (size_t i = size; i < kMaxProbe; ++i)
        {
            if (buf[i] != kGuard)
            {
                th::report_failure("encoder wrote past the size it was given", __FILE__,
                                   __LINE__);
                std::printf("    %s at size %zu, byte %zu\n", name, size, i);
                return;
            }
        }

        if (r.ok())
        {
            if (r.value() > size)
            {
                th::report_failure("encoder reported more bytes than the buffer held", __FILE__,
                                   __LINE__);
                std::printf("    %s at size %zu reported %zu\n", name, size, r.value());
                return;
            }
            if (r.value() != full.value() || std::memcmp(buf, reference, r.value()) != 0)
            {
                th::report_failure("a smaller buffer produced different bytes", __FILE__,
                                   __LINE__);
                std::printf("    %s at size %zu\n", name, size);
                return;
            }
        }
        else if (!bounded_buffer_error(r.error()))
        {
            th::report_failure("unexpected error from a short buffer", __FILE__, __LINE__);
            std::printf("    %s at size %zu: %s\n", name, size, to_string(r.error()));
            return;
        }
    }
    ++th::checks();   // one check per encoder that survived the whole sweep
}

}   // namespace

TEST(encoders_never_write_past_the_buffer_they_are_given)
{
    const uint8_t payload[]   = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    const uint8_t password[]  = {'p', 'w'};
    const uint8_t will_body[] = {0xDE, 0xAD};

    ConnectOptions bare;
    bare.client_id = etl::string_view("cid");

    ConnectOptions full;
    full.client_id     = etl::string_view("cid");
    full.username      = etl::string_view("user");
    full.password      = etl::span<const uint8_t>(password, sizeof(password));
    full.will.topic    = etl::string_view("will/topic");
    full.will.payload  = etl::span<const uint8_t>(will_body, sizeof(will_body));
    full.will.qos      = QoS::AtLeastOnce;
    full.will.retain   = true;
    full.clean_session = false;

    sweep("CONNECT", [&](etl::span<uint8_t> out) { return codec::encode_connect(out, bare); });
    sweep("CONNECT with will and credentials",
          [&](etl::span<uint8_t> out) { return codec::encode_connect(out, full); });

    sweep("PUBLISH qos0", [&](etl::span<uint8_t> out) {
        return codec::encode_publish(out, etl::string_view("a/b"),
                                     etl::span<const uint8_t>(payload, sizeof(payload)),
                                     QoS::AtMostOnce, false, false, 0);
    });
    sweep("PUBLISH qos2 retained", [&](etl::span<uint8_t> out) {
        return codec::encode_publish(out, etl::string_view("a/b/c"),
                                     etl::span<const uint8_t>(payload, sizeof(payload)),
                                     QoS::ExactlyOnce, true, true, 0x1234);
    });
    sweep("PUBLISH empty payload", [&](etl::span<uint8_t> out) {
        return codec::encode_publish(out, etl::string_view("t"), etl::span<const uint8_t>(),
                                     QoS::AtMostOnce, false, false, 0);
    });

    for (const PacketType t :
         {PacketType::Puback, PacketType::Pubrec, PacketType::Pubrel, PacketType::Pubcomp})
    {
        sweep(to_string(t),
              [&](etl::span<uint8_t> out) { return codec::encode_ack(out, t, 0x1234); });
    }

    const TopicSubscription one[1] = {{etl::string_view("a/b"), QoS::AtLeastOnce}};
    const TopicSubscription two[2] = {{etl::string_view("a/b"), QoS::AtLeastOnce},
                                      {etl::string_view("c/+/d"), QoS::ExactlyOnce}};
    sweep("SUBSCRIBE one filter", [&](etl::span<uint8_t> out) {
        return codec::encode_subscribe(out, 1, etl::span<const TopicSubscription>(one, 1));
    });
    sweep("SUBSCRIBE two filters", [&](etl::span<uint8_t> out) {
        return codec::encode_subscribe(out, 2, etl::span<const TopicSubscription>(two, 2));
    });

    const etl::string_view filters[2] = {etl::string_view("a/b"), etl::string_view("c/+/d")};
    sweep("UNSUBSCRIBE one filter", [&](etl::span<uint8_t> out) {
        return codec::encode_unsubscribe(out, 3, etl::span<const etl::string_view>(filters, 1));
    });
    sweep("UNSUBSCRIBE two filters", [&](etl::span<uint8_t> out) {
        return codec::encode_unsubscribe(out, 4, etl::span<const etl::string_view>(filters, 2));
    });

    sweep("PINGREQ", [&](etl::span<uint8_t> out) {
        return codec::encode_empty(out, PacketType::Pingreq);
    });
    sweep("DISCONNECT", [&](etl::span<uint8_t> out) {
        return codec::encode_empty(out, PacketType::Disconnect);
    });
}

// A multi-byte remaining length is the case where the fixed header itself grows,
// so the header write can fail after the type byte has already gone in.
TEST(encoders_handle_a_two_byte_remaining_length_at_every_size)
{
    static uint8_t big_payload[200];
    for (uint8_t& b : big_payload)
        b = 0x5A;

    uint8_t              reference[256] = {};
    const Result<size_t> full           = codec::encode_publish(
        etl::span<uint8_t>(reference, sizeof(reference)), etl::string_view("t"),
        etl::span<const uint8_t>(big_payload, sizeof(big_payload)), QoS::AtMostOnce, false,
        false, 0);
    REQUIRE(full.ok());
    // 1 type + 2 vbi + (2 len + 1 topic) + 200 payload
    CHECK_EQ(full.value(), size_t{206});

    for (size_t size = 0; size <= 210; ++size)
    {
        uint8_t arena[8 + 210 + 8];
        std::memset(arena, kGuard, sizeof(arena));
        uint8_t* buf = arena + 8;

        const Result<size_t> r =
            codec::encode_publish(etl::span<uint8_t>(buf, size), etl::string_view("t"),
                                  etl::span<const uint8_t>(big_payload, sizeof(big_payload)),
                                  QoS::AtMostOnce, false, false, 0);

        for (size_t i = size; i < 210; ++i)
        {
            if (buf[i] != kGuard)
            {
                th::report_failure("wrote past the buffer with a 2-byte length", __FILE__,
                                   __LINE__);
                std::printf("    size %zu, byte %zu\n", size, i);
                return;
            }
        }
        if (r.ok())
            CHECK_EQ(r.value(), full.value());
        else
            CHECK(bounded_buffer_error(r.error()));
    }
}
