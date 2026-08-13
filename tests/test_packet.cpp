#include "test_harness.hpp"

#include "mqtt/packet.hpp"

using namespace mqtt;

namespace {

// The boundary values from MQTT 3.1.1 table 2.4.
struct VbiVector
{
    uint32_t value;
    uint8_t  bytes[4];
    size_t   len;
};

const VbiVector kVectors[] = {
    {0u,         {0x00, 0, 0, 0},                1},
    {127u,       {0x7F, 0, 0, 0},                1},
    {128u,       {0x80, 0x01, 0, 0},             2},
    {16383u,     {0xFF, 0x7F, 0, 0},             2},
    {16384u,     {0x80, 0x80, 0x01, 0},          3},
    {2097151u,   {0xFF, 0xFF, 0x7F, 0},          3},
    {2097152u,   {0x80, 0x80, 0x80, 0x01},       4},
    {268435455u, {0xFF, 0xFF, 0xFF, 0x7F},       4},
};

} // namespace

TEST(vbi_encodes_spec_boundaries)
{
    for (const VbiVector& v : kVectors)
    {
        uint8_t buf[4] = {};
        const Result<size_t> n = vbi_encode(etl::span<uint8_t>(buf, 4), v.value);
        REQUIRE(n.ok());
        CHECK_EQ(n.value(), v.len);
        CHECK_BYTES(buf, n.value(), v.bytes, v.len);
        CHECK_EQ(vbi_size(v.value), v.len);
    }
}

TEST(vbi_decodes_spec_boundaries)
{
    for (const VbiVector& v : kVectors)
    {
        const Result<VbiDecode> d = vbi_decode(etl::span<const uint8_t>(v.bytes, v.len));
        REQUIRE(d.ok());
        CHECK_EQ(d.value().value, v.value);
        CHECK_EQ(d.value().bytes, v.len);
    }
}

TEST(vbi_rejects_out_of_range_value)
{
    uint8_t buf[4] = {};
    const Result<size_t> n = vbi_encode(etl::span<uint8_t>(buf, 4), 268435456u);
    CHECK(!n.ok());
    CHECK(n.error() == Error::InvalidArgument);
    CHECK_EQ(vbi_size(268435456u), size_t{0});
}

TEST(vbi_encode_reports_small_buffer)
{
    uint8_t buf[1] = {};
    const Result<size_t> n = vbi_encode(etl::span<uint8_t>(buf, 1), 128u);
    CHECK(!n.ok());
    CHECK(n.error() == Error::BufferTooSmall);
}

TEST(vbi_decode_reports_incomplete_prefix)
{
    // A continuation byte with nothing after it is a valid prefix, not garbage.
    const uint8_t buf[] = {0x80};
    const Result<VbiDecode> d = vbi_decode(etl::span<const uint8_t>(buf, 1));
    CHECK(!d.ok());
    CHECK(d.error() == Error::Incomplete);

    const Result<VbiDecode> empty = vbi_decode(etl::span<const uint8_t>());
    CHECK(!empty.ok());
    CHECK(empty.error() == Error::Incomplete);
}

TEST(vbi_decode_rejects_five_byte_encoding)
{
    const uint8_t buf[] = {0x80, 0x80, 0x80, 0x80, 0x01};
    const Result<VbiDecode> d = vbi_decode(etl::span<const uint8_t>(buf, 5));
    CHECK(!d.ok());
    CHECK(d.error() == Error::MalformedPacket);
}

TEST(vbi_decode_rejects_non_minimal_encoding)
{
    // 0x80 0x00 decodes to 0, which is representable in one byte. Paho accepts
    // this; we reject it, because accepting it lets a peer desynchronise the
    // stream framing.
    const uint8_t buf[] = {0x80, 0x00};
    const Result<VbiDecode> d = vbi_decode(etl::span<const uint8_t>(buf, 2));
    CHECK(!d.ok());
    CHECK(d.error() == Error::MalformedPacket);
}

TEST(fixed_header_roundtrips_publish_flags)
{
    FixedHeader h;
    h.type             = PacketType::Publish;
    h.dup              = true;
    h.qos              = QoS::ExactlyOnce;
    h.retain           = true;
    h.remaining_length = 10;

    const uint8_t byte = h.to_byte();
    CHECK_EQ(byte, uint8_t{0x3D});   // 0011 1101

    const Result<FixedHeader> back = FixedHeader::from_byte(byte);
    REQUIRE(back.ok());
    CHECK(back.value().type == PacketType::Publish);
    CHECK(back.value().dup);
    CHECK(back.value().qos == QoS::ExactlyOnce);
    CHECK(back.value().retain);
}

TEST(fixed_header_enforces_mandated_flag_bits)
{
    // PUBREL, SUBSCRIBE and UNSUBSCRIBE must carry flags 0b0010.
    const FixedHeader pubrel{PacketType::Pubrel, false, QoS::AtMostOnce, false, 2};
    const FixedHeader sub{PacketType::Subscribe, false, QoS::AtMostOnce, false, 0};
    const FixedHeader unsub{PacketType::Unsubscribe, false, QoS::AtMostOnce, false, 0};

    CHECK_EQ(pubrel.to_byte(), uint8_t{0x62});
    CHECK_EQ(sub.to_byte(), uint8_t{0x82});
    CHECK_EQ(unsub.to_byte(), uint8_t{0xA2});

    // Wrong flags are rejected rather than silently accepted.
    CHECK(!FixedHeader::from_byte(0x60).ok());   // PUBREL with flags 0
    CHECK(!FixedHeader::from_byte(0x81).ok());   // SUBSCRIBE with flags 1
    CHECK(!FixedHeader::from_byte(0x21).ok());   // CONNACK with flags 1
    CHECK(FixedHeader::from_byte(0x20).ok());    // CONNACK with flags 0
}

TEST(fixed_header_rejects_reserved_and_bad_qos)
{
    CHECK(!FixedHeader::from_byte(0x00).ok());   // type 0 is reserved
    CHECK(!FixedHeader::from_byte(0xF0).ok());   // type 15 is out of range for 3.1.1
    CHECK(!FixedHeader::from_byte(0x36).ok());   // PUBLISH with QoS 3
}

TEST(fixed_header_sizes_account_for_vbi)
{
    FixedHeader h;
    h.type             = PacketType::Publish;
    h.remaining_length = 127;
    CHECK_EQ(h.header_size(), size_t{2});
    CHECK_EQ(h.total_size(), size_t{129});

    h.remaining_length = 128;
    CHECK_EQ(h.header_size(), size_t{3});
    CHECK_EQ(h.total_size(), size_t{131});
}
