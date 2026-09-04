// Wraparound tests.
//
// Two counters in this library wrap, both of them reachable by a device that is
// only left running:
//
//   - the MQTT packet id, which is 16 bits and must skip 0, so it wraps from
//     65535 to 1 after 65535 QoS > 0 publishes -- about 18 hours at one per
//     second;
//   - the millisecond clock, which is 32 bits and wraps every 49.7 days. Five
//     fields read it, and elapsed_ms() relies on unsigned subtraction being
//     well defined rather than on the clock being monotonic in the large.
//
// Neither wrap was exercised anywhere else in the suite. These cases drive the
// counters to the boundary rather than asserting that the arithmetic looks
// right, because the arithmetic is not the part that fails -- the interaction
// with a table that still holds the old value is.

#include "test_harness.hpp"

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

namespace {

struct WrapConfig : DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 256;
    static constexpr size_t   tx_buffer_size         = 256;
    static constexpr size_t   max_topic_len          = 16;
    static constexpr size_t   max_inflight_out       = 2;
    static constexpr size_t   max_persisted_msg_size = 48;
    static constexpr size_t   max_inflight_in        = 2;
    static constexpr size_t   max_subscriptions      = 2;
    static constexpr size_t   max_pending_acks       = 2;
    static constexpr size_t   max_topics_per_request = 1;
    static constexpr uint32_t retry_interval_ms      = 1000;
    static constexpr uint32_t connect_timeout_ms     = 5000;
};

using WrapClient = Client<WrapConfig>;

struct Fixture
{
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    WrapClient           client{transport, clock};

    ConnectOptions default_options() const noexcept
    {
        ConnectOptions opts;
        opts.client_id     = etl::string_view("cid");
        opts.keep_alive_s  = 10;
        opts.clean_session = true;
        return opts;
    }

    bool bring_up() noexcept
    {
        if (client.connect(default_options()) != Error::Ok)
            return false;
        client.step();
        sim::push_connack(transport, false);
        client.step();
        transport.clear_sent();
        return client.is_connected();
    }
};

/// The packet id inside a PUBLISH the client sent. The body is
/// [topic length][topic][packet id], so the id sits just past the topic.
uint16_t publish_packet_id(const sim::SentPacket& p) noexcept
{
    if (!p.valid || p.body_len < 4)
        return 0;
    const size_t topic_len = (static_cast<size_t>(p.body[0]) << 8) | p.body[1];
    if (p.body_len < 2 + topic_len + 2)
        return 0;
    return static_cast<uint16_t>((static_cast<uint16_t>(p.body[2 + topic_len]) << 8) |
                                 p.body[2 + topic_len + 1]);
}

/// Publish once at QoS 1 and return the id the client chose. The caller decides
/// whether to acknowledge it.
uint16_t publish_once(Fixture& f) noexcept
{
    f.transport.clear_sent();
    if (f.client.publish(etl::string_view("t"), etl::string_view("x"), QoS::AtLeastOnce) !=
        Error::Ok)
        return 0;
    f.client.step();
    return publish_packet_id(sim::find_sent(f.transport, PacketType::Publish));
}

/// Publish, acknowledge, and return the id used. Leaves both pipes empty so the
/// loop below can run 65535 times inside a 4 KiB fake transport.
uint16_t publish_and_ack(Fixture& f) noexcept
{
    const uint16_t id = publish_once(f);
    if (id == 0)
        return 0;
    sim::push_ack(f.transport, PacketType::Puback, id);
    f.client.step();
    f.transport.clear_sent();
    f.transport.clear_inbound();
    return id;
}

}   // namespace

//------------------------------------------------------------------------------
// Packet id
//------------------------------------------------------------------------------

TEST(packet_ids_never_use_zero_and_wrap_from_65535_to_one)
{
    Fixture f;
    REQUIRE(f.bring_up());

    // Walk the whole 16-bit space. MQTT-2.3.1-1 forbids packet id 0, so a
    // correct allocator visits 65535 values and then starts again at 1.
    uint16_t previous = 0;
    bool     wrapped  = false;
    int      zeros    = 0;

    for (uint32_t i = 0; i < 65536u; ++i)
    {
        const uint16_t id = publish_and_ack(f);
        if (id == 0)
        {
            ++zeros;
            break;
        }

        if (previous != 0 && id < previous)
            wrapped = true;   // the only decrease in the sequence is the wrap

        previous = id;
    }

    CHECK_EQ(zeros, 0);
    CHECK(wrapped);
    CHECK_EQ(static_cast<int>(previous), 1);   // 65536 publishes lands back on 1
}

TEST(an_id_still_in_flight_is_not_reissued_across_the_wrap)
{
    Fixture f;
    REQUIRE(f.bring_up());

    // Advance to the point where the next two ids are 65534 and 65535.
    for (uint32_t i = 0; i < 65533u; ++i)
    {
        if (publish_and_ack(f) == 0)
            break;
    }

    const uint16_t a = publish_once(f);   // 65534, left unacknowledged
    const uint16_t b = publish_once(f);   // 65535, left unacknowledged
    CHECK_EQ(static_cast<int>(a), 65534);
    CHECK_EQ(static_cast<int>(b), 65535);

    // The window is full at two, so free the older one only. 65535 stays in
    // flight across the wrap.
    sim::push_ack(f.transport, PacketType::Puback, a);
    f.client.step();

    const uint16_t c = publish_once(f);
    CHECK_EQ(static_cast<int>(c), 1);   // wrapped, and did not reuse 65535

    // Both outstanding ids still resolve to their own message.
    sim::push_ack(f.transport, PacketType::Puback, b);
    f.client.step();
    sim::push_ack(f.transport, PacketType::Puback, c);
    f.client.step();

    CHECK(f.client.is_connected());
    CHECK_EQ(static_cast<int>(f.client.inflight_count()), 0);
}

TEST(a_qos2_handshake_straddling_the_wrap_completes)
{
    Fixture f;
    REQUIRE(f.bring_up());

    for (uint32_t i = 0; i < 65534u; ++i)
    {
        if (publish_and_ack(f) == 0)
            break;
    }

    // This one takes 65535 and runs the four-packet handshake across the wrap.
    f.transport.clear_sent();
    REQUIRE(f.client.publish(etl::string_view("t"), etl::string_view("x"), QoS::ExactlyOnce) ==
            Error::Ok);
    f.client.step();
    const uint16_t id = publish_packet_id(sim::find_sent(f.transport, PacketType::Publish));
    CHECK_EQ(static_cast<int>(id), 65535);

    sim::push_ack(f.transport, PacketType::Pubrec, id);
    f.client.step();
    CHECK(sim::find_sent(f.transport, PacketType::Pubrel).valid);

    sim::push_ack(f.transport, PacketType::Pubcomp, id);
    f.client.step();

    CHECK(f.client.is_connected());
    CHECK_EQ(static_cast<int>(f.client.inflight_count()), 0);

    // The next id is 1, not 0 and not a repeat of the completed handshake.
    CHECK_EQ(static_cast<int>(publish_once(f)), 1);
}

//------------------------------------------------------------------------------
// The 32-bit millisecond clock
//------------------------------------------------------------------------------

/// Start every timer 5 s below 2^32 so that the scenarios below cross the
/// boundary in the middle rather than at a step edge.
constexpr uint32_t kNearWrap = 0xFFFFFFFFu - 5000u;

TEST(keep_alive_pings_once_across_the_clock_wrap)
{
    Fixture f;
    f.clock.now = kNearWrap;
    REQUIRE(f.bring_up());

    // 10 s keep-alive pings at 75%, so 7500 ms. Crossing 2^32 on the way.
    f.clock.advance(7000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{0});

    f.clock.advance(600);   // now past the threshold, and past the wrap
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{1});

    // A second step must not ping again while the first is outstanding.
    f.clock.advance(100);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{1});

    sim::push_pingresp(f.transport);
    f.client.step();
    CHECK(f.client.is_connected());
}

TEST(keep_alive_still_times_out_across_the_clock_wrap)
{
    Fixture f;
    f.clock.now = kNearWrap;
    REQUIRE(f.bring_up());

    f.clock.advance(7600);   // ping goes out, wrapping the clock
    f.client.step();
    REQUIRE(sim::count_sent(f.transport, PacketType::Pingreq) == size_t{1});

    // No PINGRESP. One keep-alive period later the session must end, and the
    // wrap must not turn that into a spurious survival.
    f.clock.advance(10001);
    const Error e = f.client.step();

    CHECK(e == Error::KeepAliveTimeout);
    CHECK(!f.client.is_connected());
}

TEST(retransmission_fires_once_across_the_clock_wrap)
{
    Fixture f;
    f.clock.now = kNearWrap;
    REQUIRE(f.bring_up());

    f.transport.clear_sent();
    REQUIRE(f.client.publish(etl::string_view("t"), etl::string_view("x"), QoS::AtLeastOnce) ==
            Error::Ok);
    f.client.step();
    REQUIRE(sim::count_sent(f.transport, PacketType::Publish) == size_t{1});

    // retry_interval_ms is 1000. Just short of it, nothing; past it, exactly
    // one retransmission, with the boundary crossed in between.
    f.clock.advance(900);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Publish), size_t{1});

    f.clock.advance(200);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Publish), size_t{2});

    // A gap of many retry intervals is still one retransmission, not one per
    // interval, because the slot is timed rather than counted.
    f.clock.advance(50000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Publish), size_t{3});
}

TEST(connect_timeout_measures_correctly_across_the_clock_wrap)
{
    Fixture f;
    f.clock.now = kNearWrap;

    REQUIRE(f.client.connect(f.default_options()) == Error::Ok);
    f.client.step();
    REQUIRE(f.client.state() == State::AwaitingConnack);

    // connect_timeout_ms is 5000, so this crosses 2^32 while still inside it.
    f.clock.advance(4900);
    CHECK(f.client.step() == Error::Ok);
    CHECK(f.client.state() == State::AwaitingConnack);

    f.clock.advance(200);
    CHECK(f.client.step() == Error::ConnectTimeout);
    CHECK(!f.client.is_connected());
}

TEST(ms_since_last_receive_stays_small_across_the_clock_wrap)
{
    Fixture f;
    f.clock.now = kNearWrap;
    REQUIRE(f.bring_up());

    // The CONNACK arrived just before the wrap; step past it.
    f.clock.advance(6000);
    f.client.step();

    // A value near 2^32 here would mean the subtraction had been read as
    // signed, or the field reset. It is the number a caller hangs its own
    // watchdog on, so it has to survive the boundary.
    CHECK(f.client.ms_since_last_receive() >= 6000u);
    CHECK(f.client.ms_since_last_receive() < 20000u);

    sim::push_pingresp(f.transport);
    f.client.step();
    CHECK(f.client.ms_since_last_receive() < 1000u);
}
