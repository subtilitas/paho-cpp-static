// MQTT UTF-8 encoded strings (MQTT 3.1.1 section 1.5.3).
//
// The vectors below are the usual suspects from Markus Kuhn's UTF-8 decoder
// stress test, trimmed to what a protocol implementation can actually be
// handed. The overlong forms are the ones worth caring about: they let one
// value be written several ways, so a broker access-control rule matching on
// bytes and a client decoding to characters can disagree about which topic is
// under discussion.
//
// Every vector is a *named* array. A string_view over a braced temporary would
// dangle the moment its full-expression ended, and the bug would show up as a
// flake rather than a failure.

#include "test_harness.hpp"

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"
#include "mqtt/codec.hpp"
#include "mqtt/topic.hpp"
#include "mqtt/utf8.hpp"

using namespace mqtt;

namespace {

/// Wire bytes as uint8_t, the way the codec tests write them, rather than as
/// char literals. '\xC0' is a char whose value is implementation-defined, and
/// MSVC and GCC disagree about how loudly to say so; a uint8_t table says
/// exactly what is on the wire and casts once, here.
template <size_t N>
etl::string_view view(const uint8_t (&bytes)[N]) noexcept
{
    return etl::string_view(reinterpret_cast<const char*>(bytes), N);
}

// --- well formed -------------------------------------------------------------
const uint8_t kAsciiTopic[] = {0x73, 0x65, 0x6E, 0x73, 0x6F, 0x72, 0x73, 0x2F, 0x37};
const uint8_t kDel[] = {0x7F};                                    // U+007F
const uint8_t kTwoByteMin[] = {0xC2, 0x80};                            // U+0080
const uint8_t kTwoByteMax[] = {0xDF, 0xBF};                            // U+07FF
const uint8_t kThreeByteMin[] = {0xE0, 0xA0, 0x80};                    // U+0800
const uint8_t kReplacement[] = {0xEF, 0xBF, 0xBD};                    // U+FFFD
const uint8_t kUmlaut[] = {0xC3, 0xA4};                            // U+00E4
const uint8_t kFourByteMin[] = {0xF0, 0x90, 0x80, 0x80};            // U+10000
const uint8_t kMaxCodePoint[] = {0xF4, 0x8F, 0xBF, 0xBF};            // U+10FFFF
const uint8_t kByteOrderMark[] = {0xEF, 0xBB, 0xBF};                   // U+FEFF
const uint8_t kControl01[] = {0x01};
const uint8_t kControl1F[] = {0x1F};

// --- forbidden by MQTT rather than by UTF-8 ----------------------------------
const uint8_t kNul[] = {0x00};
const uint8_t kNulInside[] = {0x61, 0x00, 0x62};

// --- overlong ----------------------------------------------------------------
const uint8_t kOverlongNul2[] = {0xC0, 0x80};                            // U+0000 as 2
const uint8_t kOverlongSlash[] = {0xC0, 0xAF};                           // '/' as 2
const uint8_t kOverlongDel[] = {0xC1, 0xBF};                            // U+007F as 2
const uint8_t kOverlongNul3[] = {0xE0, 0x80, 0x80};
const uint8_t kOverlong07FF[] = {0xE0, 0x9F, 0xBF};
const uint8_t kOverlongNul4[] = {0xF0, 0x80, 0x80, 0x80};
const uint8_t kOverlongFFFF[] = {0xF0, 0x8F, 0xBF, 0xBF};

// --- surrogates and out of range ---------------------------------------------
const uint8_t kSurrogateLow[] = {0xED, 0xA0, 0x80};                   // U+D800
const uint8_t kSurrogateHigh[] = {0xED, 0xBF, 0xBF};                   // U+DFFF
const uint8_t kAboveMax[] = {0xF4, 0x90, 0x80, 0x80};           // U+110000
const uint8_t kWayAboveMax[] = {0xF7, 0xBF, 0xBF, 0xBF};           // U+1FFFFF

// --- structurally malformed ---------------------------------------------------
const uint8_t kLoneCont80[] = {0x80};
const uint8_t kLoneContBF[] = {0xBF};
const uint8_t kTruncated2[] = {0xC2};
const uint8_t kTruncated3[] = {0xE0, 0xA0};
const uint8_t kTruncated4[] = {0xF0, 0x90, 0x80};
const uint8_t kBadCont[] = {0xC2, 0x41};
const uint8_t kFE[] = {0xFE};
const uint8_t kFF[] = {0xFF};
const uint8_t kFiveByte[] = {0xFB, 0xBF, 0xBF, 0xBF, 0xBF};
const uint8_t kSixByte[] = {0xFD, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF};
const uint8_t kGoodThenBad[] = {0x6F, 0x6B, 0xC0, 0x80};

// --- used against the topic predicates and the encoder ------------------------
const uint8_t kBadTopic[] = {0x61, 0x2F, 0xC0, 0x80};
const uint8_t kBadFilter[] = {0x61, 0x2F, 0xC0, 0xAF};
const uint8_t kBadString[] = {0x62, 0x61, 0x64, 0xC0, 0x80};

}   // namespace

//------------------------------------------------------------------------------
// The validator
//------------------------------------------------------------------------------

TEST(utf8_accepts_well_formed_sequences)
{
    CHECK(is_valid_mqtt_string(etl::string_view("")));
    CHECK(is_valid_mqtt_string(view(kAsciiTopic)));
    CHECK(is_valid_mqtt_string(view(kDel)));
    CHECK(is_valid_mqtt_string(view(kTwoByteMin)));
    CHECK(is_valid_mqtt_string(view(kTwoByteMax)));
    CHECK(is_valid_mqtt_string(view(kThreeByteMin)));
    CHECK(is_valid_mqtt_string(view(kReplacement)));
    CHECK(is_valid_mqtt_string(view(kUmlaut)));
    CHECK(is_valid_mqtt_string(view(kFourByteMin)));
    CHECK(is_valid_mqtt_string(view(kMaxCodePoint)));
}

TEST(utf8_accepts_what_the_spec_only_discourages)
{
    // MQTT-1.5.3-3 says a receiver must not skip or strip a byte order mark,
    // which means it has to be willing to accept one first.
    CHECK(is_valid_mqtt_string(view(kByteOrderMark)));

    // Control characters are a SHOULD NOT, not a MUST NOT. Rejecting on a
    // SHOULD would drop traffic a conforming peer is entitled to send.
    CHECK(is_valid_mqtt_string(view(kControl01)));
    CHECK(is_valid_mqtt_string(view(kControl1F)));
}

TEST(utf8_rejects_the_null_character)
{
    // MQTT-1.5.3-2. Well-formed UTF-8 and still forbidden, which is why it is
    // checked separately from the encoding rules.
    CHECK(!is_valid_mqtt_string(view(kNul)));
    CHECK(!is_valid_mqtt_string(view(kNulInside)));
}

TEST(utf8_rejects_overlong_encodings)
{
    // The same rule vbi_decode applies to variable byte integers: the shortest
    // spelling is the only legal one, because two spellings of one value is an
    // ambiguity somebody else gets to choose between.
    CHECK(!is_valid_mqtt_string(view(kOverlongNul2)));
    CHECK(!is_valid_mqtt_string(view(kOverlongSlash)));
    CHECK(!is_valid_mqtt_string(view(kOverlongDel)));
    CHECK(!is_valid_mqtt_string(view(kOverlongNul3)));
    CHECK(!is_valid_mqtt_string(view(kOverlong07FF)));
    CHECK(!is_valid_mqtt_string(view(kOverlongNul4)));
    CHECK(!is_valid_mqtt_string(view(kOverlongFFFF)));
}

TEST(utf8_rejects_surrogates_and_out_of_range_code_points)
{
    CHECK(!is_valid_mqtt_string(view(kSurrogateLow)));
    CHECK(!is_valid_mqtt_string(view(kSurrogateHigh)));
    CHECK(!is_valid_mqtt_string(view(kAboveMax)));
    CHECK(!is_valid_mqtt_string(view(kWayAboveMax)));
}

TEST(utf8_rejects_structurally_malformed_sequences)
{
    CHECK(!is_valid_mqtt_string(view(kLoneCont80)));
    CHECK(!is_valid_mqtt_string(view(kLoneContBF)));
    CHECK(!is_valid_mqtt_string(view(kTruncated2)));
    CHECK(!is_valid_mqtt_string(view(kTruncated3)));
    CHECK(!is_valid_mqtt_string(view(kTruncated4)));
    CHECK(!is_valid_mqtt_string(view(kBadCont)));
    CHECK(!is_valid_mqtt_string(view(kFE)));
    CHECK(!is_valid_mqtt_string(view(kFF)));

    // The five- and six-byte forms RFC 3629 removed when Unicode was capped.
    CHECK(!is_valid_mqtt_string(view(kFiveByte)));
    CHECK(!is_valid_mqtt_string(view(kSixByte)));

    // A good prefix must not excuse a bad tail.
    CHECK(!is_valid_mqtt_string(view(kGoodThenBad)));
}

//------------------------------------------------------------------------------
// The topic predicates
//------------------------------------------------------------------------------

TEST(topic_validation_rejects_malformed_utf8)
{
    CHECK(is_valid_topic_name(etl::string_view("a/b")));
    CHECK(!is_valid_topic_name(view(kBadTopic)));
    CHECK(!is_valid_topic_name(view(kSurrogateLow)));

    CHECK(is_valid_filter(etl::string_view("a/+/c")));
    CHECK(!is_valid_filter(view(kBadFilter)));
}

//------------------------------------------------------------------------------
// The encoder rejects what the decoder rejects
//------------------------------------------------------------------------------

TEST(encoder_refuses_to_emit_a_malformed_string)
{
    // Same principle as the DUP-at-QoS-0 and QoS(3) checks: these entry points
    // are documented as API, so they must not build a packet the peer's
    // decoder is required to reject.
    const etl::string_view bad = view(kBadString);

    CHECK(codec::publish_remaining_length(bad, 1, QoS::AtMostOnce).error()
          == Error::InvalidArgument);

    ConnectOptions opts;
    opts.client_id = bad;
    CHECK(codec::connect_remaining_length(opts).error() == Error::InvalidArgument);

    opts.client_id = etl::string_view("cid");
    opts.username  = bad;
    CHECK(codec::connect_remaining_length(opts).error() == Error::InvalidArgument);

    static const uint8_t payload[] = {'x'};
    opts.username     = etl::string_view();
    opts.will.topic   = bad;
    opts.will.payload = etl::span<const uint8_t>(payload, 1);
    CHECK(codec::connect_remaining_length(opts).error() == Error::InvalidArgument);

    const TopicSubscription sub{bad, QoS::AtMostOnce};
    CHECK(codec::subscribe_remaining_length(
              etl::span<const TopicSubscription>(&sub, 1)).error()
          == Error::InvalidArgument);

    CHECK(codec::unsubscribe_remaining_length(
              etl::span<const etl::string_view>(&bad, 1)).error()
          == Error::InvalidArgument);
}

//------------------------------------------------------------------------------
// The inbound path -- the half that matters
//------------------------------------------------------------------------------

namespace {

struct Utf8Config : DefaultConfig
{
    static constexpr size_t rx_buffer_size   = 256;
    static constexpr size_t tx_buffer_size   = 256;
    static constexpr size_t max_inflight_out = 0;
};

ConnectOptions utf8_options() noexcept
{
    ConnectOptions opts;
    opts.client_id     = etl::string_view("cid");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;
    return opts;
}

}   // namespace

TEST(a_publish_with_a_malformed_topic_ends_the_session)
{
    // MQTT-1.5.3-1 requires the receiver to close the connection rather than
    // deliver it. Without this the application is handed a view over bytes it
    // will treat as text -- the one case here where being lenient costs
    // somebody else something.
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    Client<Utf8Config>   client{transport, clock};

    int  delivered = 0;
    auto on_msg    = [&](const Message&) { ++delivered; };
    client.on_message(on_msg);

    CHECK(client.connect(utf8_options()) == Error::Ok);
    client.step();
    sim::push_connack(transport, false);
    client.step();
    REQUIRE(client.is_connected());

    // PUBLISH whose topic is an overlong encoding of '/'. Built by hand:
    // push_publish takes a NUL-terminated C string, and this is not one.
    static const uint8_t packet[] = {
        0x30,               // PUBLISH, QoS 0
        0x07,               // remaining length
        0x00, 0x03,         // topic length 3
        0xC0, 0xAF,         // overlong '/'
        0x61,               // 'a'
        0x78, 0x79,         // payload "xy"
    };
    transport.push_inbound(packet, sizeof packet);

    const Error e = client.step();

    CHECK_EQ(delivered, 0);
    CHECK(e == Error::MalformedPacket);
    CHECK(!client.is_connected());
}

TEST(a_publish_with_a_well_formed_multibyte_topic_is_delivered)
{
    // The other half of the claim: strictness must not cost correctness.
    // "a/ä" is perfectly legal and has to arrive.
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    Client<Utf8Config>   client{transport, clock};

    int  delivered = 0;
    auto on_msg    = [&](const Message&) { ++delivered; };
    client.on_message(on_msg);

    CHECK(client.connect(utf8_options()) == Error::Ok);
    client.step();
    sim::push_connack(transport, false);
    client.step();
    REQUIRE(client.is_connected());

    static const uint8_t packet[] = {
        0x30,               // PUBLISH, QoS 0
        0x08,               // remaining length
        0x00, 0x04,         // topic length 4
        0x61, 0x2F,         // "a/"
        0xC3, 0xA4,         // U+00E4
        0x78, 0x79,         // payload "xy"
    };
    transport.push_inbound(packet, sizeof packet);

    CHECK(client.step() == Error::Ok);
    CHECK_EQ(delivered, 1);
    CHECK(client.is_connected());
}
