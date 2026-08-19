#include "test_harness.hpp"

#include "mqtt/codec.hpp"

using namespace mqtt;

TEST(codec_connect_matches_golden_bytes)
{
    ConnectOptions opts;
    opts.client_id     = etl::string_view("abc");
    opts.keep_alive_s  = 60;
    opts.clean_session = true;

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_connect(etl::span<uint8_t>(buf, sizeof(buf)), opts);
    REQUIRE(n.ok());

    // Bytes straight off the wire, laid out one field per line with the
    // field named. That layout is the assertion's documentation.
    // clang-format off
    const uint8_t want[] = {
        0x10, 0x0F,                          // CONNECT, remaining length 15
        0x00, 0x04, 'M', 'Q', 'T', 'T',      // protocol name
        0x04,                                // protocol level 4 (3.1.1)
        0x02,                                // flags: clean session
        0x00, 0x3C,                          // keep alive 60
        0x00, 0x03, 'a', 'b', 'c',           // client id
    };
    // clang-format on
    CHECK_BYTES(buf, n.value(), want, sizeof(want));
}

TEST(codec_connect_sets_will_and_credential_flags)
{
    const uint8_t will_payload[] = {0xDE, 0xAD};
    const uint8_t password[]     = {'p', 'w'};

    ConnectOptions opts;
    opts.client_id     = etl::string_view("c");
    opts.username      = etl::string_view("u");
    opts.password      = etl::span<const uint8_t>(password, 2);
    opts.will.topic    = etl::string_view("w/t");
    opts.will.payload  = etl::span<const uint8_t>(will_payload, 2);
    opts.will.qos      = QoS::AtLeastOnce;
    opts.will.retain   = true;
    opts.clean_session = false;

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_connect(etl::span<uint8_t>(buf, sizeof(buf)), opts);
    REQUIRE(n.ok());

    // flags byte: username(0x80) password(0x40) willRetain(0x20)
    //             willQoS 1 (0x08) will(0x04) clean(0x00)
    CHECK_EQ(buf[9], uint8_t{0xEC});

    // Remaining length must agree with what was actually written.
    CHECK_EQ(static_cast<size_t>(buf[1]) + 2u, n.value());
}

TEST(codec_connect_rejects_password_without_username)
{
    const uint8_t password[] = {'p'};
    ConnectOptions opts;
    opts.client_id = etl::string_view("c");
    opts.password  = etl::span<const uint8_t>(password, 1);

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_connect(etl::span<uint8_t>(buf, sizeof(buf)), opts);
    CHECK(!n.ok());
    CHECK(n.error() == Error::InvalidArgument);
}

TEST(codec_connect_reports_buffer_too_small)
{
    ConnectOptions opts;
    opts.client_id = etl::string_view("abc");

    uint8_t buf[8] = {};
    const Result<size_t> n = codec::encode_connect(etl::span<uint8_t>(buf, sizeof(buf)), opts);
    CHECK(!n.ok());
    CHECK(n.error() == Error::BufferTooSmall);
}

TEST(codec_publish_qos0_roundtrips)
{
    const uint8_t payload[] = {'h', 'i'};

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_publish(
        etl::span<uint8_t>(buf, sizeof(buf)), etl::string_view("a/b"),
        etl::span<const uint8_t>(payload, 2), QoS::AtMostOnce, false, false, 0);
    REQUIRE(n.ok());

    const uint8_t want[] = {0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'};
    CHECK_BYTES(buf, n.value(), want, sizeof(want));

    const Result<codec::PacketPeek> peek =
        codec::peek_header(etl::span<const uint8_t>(buf, n.value()));
    REQUIRE(peek.ok());
    CHECK(peek.value().header.type == PacketType::Publish);
    CHECK_EQ(peek.value().total_bytes, n.value());

    const Result<Message> msg = codec::decode_publish(
        peek.value().header,
        etl::span<const uint8_t>(buf + peek.value().header_bytes,
                                 peek.value().header.remaining_length));
    REQUIRE(msg.ok());
    CHECK(msg.value().topic == etl::string_view("a/b"));
    CHECK_EQ(msg.value().payload.size(), size_t{2});
    CHECK_EQ(msg.value().payload[0], uint8_t{'h'});
    CHECK_EQ(msg.value().packet_id, uint16_t{0});
}

TEST(codec_publish_qos2_carries_packet_id)
{
    const uint8_t payload[] = {'x'};

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_publish(
        etl::span<uint8_t>(buf, sizeof(buf)), etl::string_view("t"),
        etl::span<const uint8_t>(payload, 1), QoS::ExactlyOnce, true, true, 0x1234);
    REQUIRE(n.ok());

    const uint8_t want[] = {0x3D, 0x06, 0x00, 0x01, 't', 0x12, 0x34, 'x'};
    CHECK_BYTES(buf, n.value(), want, sizeof(want));

    const Result<codec::PacketPeek> peek =
        codec::peek_header(etl::span<const uint8_t>(buf, n.value()));
    REQUIRE(peek.ok());

    const Result<Message> msg = codec::decode_publish(
        peek.value().header,
        etl::span<const uint8_t>(buf + peek.value().header_bytes,
                                 peek.value().header.remaining_length));
    REQUIRE(msg.ok());
    CHECK_EQ(msg.value().packet_id, uint16_t{0x1234});
    CHECK(msg.value().qos == QoS::ExactlyOnce);
    CHECK(msg.value().retain);
    CHECK(msg.value().dup);
    CHECK_EQ(msg.value().payload.size(), size_t{1});
}

TEST(codec_publish_rejects_qos_without_packet_id)
{
    uint8_t buf[32] = {};
    const Result<size_t> n = codec::encode_publish(
        etl::span<uint8_t>(buf, sizeof(buf)), etl::string_view("t"),
        etl::span<const uint8_t>(), QoS::AtLeastOnce, false, false, 0);
    CHECK(!n.ok());
    CHECK(n.error() == Error::InvalidArgument);
}

TEST(codec_publish_decode_rejects_empty_topic)
{
    FixedHeader h;
    h.type             = PacketType::Publish;
    h.remaining_length = 2;

    const uint8_t body[] = {0x00, 0x00};
    const Result<Message> msg =
        codec::decode_publish(h, etl::span<const uint8_t>(body, 2));
    CHECK(!msg.ok());
    CHECK(msg.error() == Error::ProtocolViolation);
}

TEST(codec_publish_decode_rejects_truncated_topic)
{
    FixedHeader h;
    h.type             = PacketType::Publish;
    h.remaining_length = 3;

    // Claims a 5-byte topic but only supplies one byte.
    const uint8_t body[] = {0x00, 0x05, 'a'};
    const Result<Message> msg =
        codec::decode_publish(h, etl::span<const uint8_t>(body, 3));
    CHECK(!msg.ok());
    CHECK(msg.error() == Error::MalformedPacket);
}

TEST(codec_publish_decode_rejects_dup_on_qos0)
{
    FixedHeader h;
    h.type             = PacketType::Publish;
    h.dup              = true;
    h.qos              = QoS::AtMostOnce;
    h.remaining_length = 3;

    const uint8_t body[] = {0x00, 0x01, 't'};
    const Result<Message> msg =
        codec::decode_publish(h, etl::span<const uint8_t>(body, 3));
    CHECK(!msg.ok());
    CHECK(msg.error() == Error::ProtocolViolation);
}

TEST(codec_acks_roundtrip)
{
    const PacketType types[] = {PacketType::Puback, PacketType::Pubrec,
                                PacketType::Pubrel, PacketType::Pubcomp};
    const uint8_t first_bytes[] = {0x40, 0x50, 0x62, 0x70};

    for (size_t i = 0; i < 4; ++i)
    {
        uint8_t buf[8] = {};
        const Result<size_t> n =
            codec::encode_ack(etl::span<uint8_t>(buf, sizeof(buf)), types[i], 0xBEEF);
        REQUIRE(n.ok());
        CHECK_EQ(n.value(), size_t{4});
        CHECK_EQ(buf[0], first_bytes[i]);
        CHECK_EQ(buf[1], uint8_t{0x02});

        const Result<uint16_t> id =
            codec::decode_packet_id(etl::span<const uint8_t>(buf + 2, 2));
        REQUIRE(id.ok());
        CHECK_EQ(id.value(), uint16_t{0xBEEF});
    }
}

TEST(codec_ack_rejects_zero_packet_id)
{
    uint8_t buf[8] = {};
    CHECK(!codec::encode_ack(etl::span<uint8_t>(buf, sizeof(buf)),
                             PacketType::Puback, 0).ok());

    const uint8_t body[] = {0x00, 0x00};
    const Result<uint16_t> id = codec::decode_packet_id(etl::span<const uint8_t>(body, 2));
    CHECK(!id.ok());
    CHECK(id.error() == Error::ProtocolViolation);
}

TEST(codec_subscribe_encodes_filters_and_qos)
{
    const TopicSubscription subs[] = {
        {etl::string_view("a/b"), QoS::AtLeastOnce},
        {etl::string_view("c"),   QoS::ExactlyOnce},
    };

    uint8_t buf[64] = {};
    const Result<size_t> n = codec::encode_subscribe(
        etl::span<uint8_t>(buf, sizeof(buf)), 7,
        etl::span<const TopicSubscription>(subs, 2));
    REQUIRE(n.ok());

    // Bytes straight off the wire, laid out one field per line with the
    // field named. That layout is the assertion's documentation.
    // clang-format off
    const uint8_t want[] = {
        0x82, 0x0C,              // SUBSCRIBE (flags 0010), remaining length 12
        0x00, 0x07,              // packet id
        0x00, 0x03, 'a', '/', 'b', 0x01,
        0x00, 0x01, 'c', 0x02,
    };
    // clang-format on
    CHECK_BYTES(buf, n.value(), want, sizeof(want));
}

TEST(codec_subscribe_rejects_empty_list)
{
    uint8_t buf[16] = {};
    const Result<size_t> n = codec::encode_subscribe(
        etl::span<uint8_t>(buf, sizeof(buf)), 1, etl::span<const TopicSubscription>());
    CHECK(!n.ok());
    CHECK(n.error() == Error::InvalidArgument);
}

TEST(codec_unsubscribe_encodes_filters)
{
    const etl::string_view filters[] = {etl::string_view("a/b")};

    uint8_t buf[32] = {};
    const Result<size_t> n = codec::encode_unsubscribe(
        etl::span<uint8_t>(buf, sizeof(buf)), 9,
        etl::span<const etl::string_view>(filters, 1));
    REQUIRE(n.ok());

    const uint8_t want[] = {0xA2, 0x07, 0x00, 0x09, 0x00, 0x03, 'a', '/', 'b'};
    CHECK_BYTES(buf, n.value(), want, sizeof(want));
}

TEST(codec_connack_decodes)
{
    const uint8_t accepted[] = {0x01, 0x00};
    const Result<ConnackInfo> a = codec::decode_connack(etl::span<const uint8_t>(accepted, 2));
    REQUIRE(a.ok());
    CHECK(a.value().session_present);
    CHECK(a.value().code == ConnackCode::Accepted);

    const uint8_t refused[] = {0x00, 0x05};
    const Result<ConnackInfo> r = codec::decode_connack(etl::span<const uint8_t>(refused, 2));
    REQUIRE(r.ok());
    CHECK(!r.value().session_present);
    CHECK(r.value().code == ConnackCode::NotAuthorized);
}

TEST(codec_connack_rejects_reserved_bits_and_bad_code)
{
    const uint8_t reserved_set[] = {0x02, 0x00};
    CHECK(codec::decode_connack(etl::span<const uint8_t>(reserved_set, 2)).error() ==
          Error::ProtocolViolation);

    const uint8_t bad_code[] = {0x00, 0x06};
    CHECK(codec::decode_connack(etl::span<const uint8_t>(bad_code, 2)).error() ==
          Error::ProtocolViolation);

    const uint8_t too_short[] = {0x00};
    CHECK(codec::decode_connack(etl::span<const uint8_t>(too_short, 1)).error() ==
          Error::MalformedPacket);
}

TEST(codec_suback_decodes_return_codes)
{
    const uint8_t body[] = {0x00, 0x0A, 0x00, 0x02, 0x80};
    const Result<codec::SubackView> v = codec::decode_suback(etl::span<const uint8_t>(body, 5));
    REQUIRE(v.ok());
    CHECK_EQ(v.value().packet_id, uint16_t{10});
    CHECK_EQ(v.value().return_codes.size(), size_t{3});
    CHECK_EQ(v.value().return_codes[0], uint8_t{0});
    CHECK_EQ(v.value().return_codes[2], kSubackFailure);
}

TEST(codec_suback_rejects_invalid_return_code)
{
    const uint8_t body[] = {0x00, 0x0A, 0x03};
    CHECK(codec::decode_suback(etl::span<const uint8_t>(body, 3)).error() ==
          Error::ProtocolViolation);

    const uint8_t no_codes[] = {0x00, 0x0A};
    CHECK(codec::decode_suback(etl::span<const uint8_t>(no_codes, 2)).error() ==
          Error::MalformedPacket);
}

TEST(codec_empty_packets)
{
    uint8_t buf[4] = {};

    const Result<size_t> ping = codec::encode_empty(etl::span<uint8_t>(buf, 4), PacketType::Pingreq);
    REQUIRE(ping.ok());
    const uint8_t want_ping[] = {0xC0, 0x00};
    CHECK_BYTES(buf, ping.value(), want_ping, 2);

    const Result<size_t> disc =
        codec::encode_empty(etl::span<uint8_t>(buf, 4), PacketType::Disconnect);
    REQUIRE(disc.ok());
    const uint8_t want_disc[] = {0xE0, 0x00};
    CHECK_BYTES(buf, disc.value(), want_disc, 2);

    CHECK(!codec::encode_empty(etl::span<uint8_t>(buf, 4), PacketType::Publish).ok());
}

TEST(codec_peek_reports_incomplete_until_header_arrives)
{
    // A 200-byte PUBLISH needs a two-byte remaining length.
    const uint8_t partial[] = {0x30, 0xC8};
    const Result<codec::PacketPeek> p1 =
        codec::peek_header(etl::span<const uint8_t>(partial, 1));
    CHECK(p1.error() == Error::Incomplete);

    const Result<codec::PacketPeek> p2 =
        codec::peek_header(etl::span<const uint8_t>(partial, 2));
    CHECK(p2.error() == Error::Incomplete);   // still needs the second VBI byte

    const uint8_t full[] = {0x30, 0xC8, 0x01};
    const Result<codec::PacketPeek> p3 = codec::peek_header(etl::span<const uint8_t>(full, 3));
    REQUIRE(p3.ok());
    CHECK_EQ(p3.value().header.remaining_length, uint32_t{200});
    CHECK_EQ(p3.value().header_bytes, size_t{3});
    CHECK_EQ(p3.value().total_bytes, size_t{203});
}

//------------------------------------------------------------------------------
// Encoder-side validation
//
// The decoder rejects these; the encoder must not be able to produce them, or
// the library can put a packet on the wire that it would refuse to receive.
//------------------------------------------------------------------------------

TEST(codec_publish_rejects_dup_on_qos0)
{
    uint8_t       buf[32]     = {};
    const uint8_t payload[1]  = {'x'};

    CHECK(codec::encode_publish(etl::span<uint8_t>(buf, sizeof(buf)),
                                etl::string_view("t"),
                                etl::span<const uint8_t>(payload, 1),
                                QoS::AtMostOnce, false, /*dup=*/true, 0)
              .error() == Error::InvalidArgument);   // MQTT-3.3.1-2

    // The same call without DUP is fine.
    CHECK(codec::encode_publish(etl::span<uint8_t>(buf, sizeof(buf)),
                                etl::string_view("t"),
                                etl::span<const uint8_t>(payload, 1),
                                QoS::AtMostOnce, false, false, 0)
              .ok());
}

TEST(codec_rejects_qos_outside_the_defined_range)
{
    const QoS bad = static_cast<QoS>(3);
    uint8_t   buf[64] = {};

    const TopicSubscription sub{etl::string_view("a"), bad};
    CHECK(codec::encode_subscribe(etl::span<uint8_t>(buf, sizeof(buf)), 1,
                                  etl::span<const TopicSubscription>(&sub, 1))
              .error() == Error::InvalidArgument);   // MQTT-3-8.3-4

    ConnectOptions opts;
    opts.client_id  = etl::string_view("c");
    opts.will.topic = etl::string_view("w");
    opts.will.qos   = bad;
    CHECK(codec::encode_connect(etl::span<uint8_t>(buf, sizeof(buf)), opts)
              .error() == Error::InvalidArgument);   // MQTT-3.1.2-14

    CHECK(codec::publish_remaining_length(etl::string_view("t"), 1, bad)
              .error() == Error::InvalidArgument);
}

TEST(codec_connack_rejects_session_present_with_a_refusal)
{
    // MQTT-3.2.2-4: a broker that refuses the connection must report no session.
    const uint8_t contradictory[] = {0x01, 0x05};
    CHECK(codec::decode_connack(etl::span<const uint8_t>(contradictory, 2))
              .error() == Error::ProtocolViolation);

    const uint8_t consistent[] = {0x00, 0x05};
    CHECK(codec::decode_connack(etl::span<const uint8_t>(consistent, 2)).ok());
}

TEST(codec_string_helpers_roundtrip)
{
    uint8_t buf[16] = {};
    etl::byte_stream_writer w(reinterpret_cast<char*>(buf), sizeof(buf), codec::kNetworkOrder);
    CHECK(codec::write_string(w, etl::string_view("hey")) == Error::Ok);
    CHECK_EQ(w.size_bytes(), size_t{5});

    etl::byte_stream_reader r(buf, w.size_bytes(), codec::kNetworkOrder);
    const Result<etl::string_view> s = codec::read_string(r);
    REQUIRE(s.ok());
    CHECK(s.value() == etl::string_view("hey"));
}
