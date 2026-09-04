// The client is a template whose whole selling point is that its capacities are
// yours to choose, and until now the suite instantiated exactly one config. So
// every branch that only fires when something is *tight* -- a transmit queue
// with no room, an inflight window of zero, retransmission switched off -- was
// untested, and a user who picked one of the profiles the documentation
// recommends was running code the suite never executed.
//
// These are the configurations at the edges of what config.hpp permits.

#include "test_harness.hpp"

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

namespace {

/// QoS 0 only, and no room to spare. This is the "sensor" profile from
/// docs/configuration.md taken to its limit.
struct TinyConfig : DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 64;
    static constexpr size_t   tx_buffer_size         = 32;
    static constexpr size_t   max_topic_len          = 16;
    static constexpr size_t   max_client_id_len      = 8;
    static constexpr size_t   max_inflight_out       = 0;   // QoS > 0 forbidden
    static constexpr size_t   max_persisted_msg_size = 0;
    static constexpr size_t   max_inflight_in        = 1;
    static constexpr size_t   max_subscriptions      = 1;
    static constexpr size_t   max_pending_acks       = 1;
    static constexpr size_t   max_topics_per_request = 1;
    static constexpr uint32_t retry_interval_ms      = 0;   // retry on reconnect only
    static constexpr uint32_t connect_timeout_ms     = 1000;
};

/// Timed retransmission switched off, which config.hpp documents as "retry only
/// on reconnect".
struct NoRetryConfig : DefaultConfig
{
    static constexpr size_t   max_inflight_out  = 2;
    static constexpr uint32_t retry_interval_ms = 0;
};

template <typename Cfg>
struct Rig
{
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    Client<Cfg>          client{transport, clock};

    /// keep_alive_s = 0 for tests that advance the clock a long way: otherwise
    /// the session dies of a keep-alive timeout before the thing under test
    /// happens, and the failure looks like the feature rather than the fixture.
    bool bring_up(uint16_t keep_alive_s = 10, bool clean_session = true) noexcept
    {
        ConnectOptions o;
        o.client_id     = etl::string_view("c");
        o.keep_alive_s  = keep_alive_s;
        o.clean_session = clean_session;
        if (client.connect(o) != Error::Ok)
            return false;
        client.step();
        sim::push_connack(transport, false);
        client.step();
        return client.is_connected();
    }
};

}   // namespace

TEST(config_with_no_inflight_window_refuses_qos_above_zero)
{
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    // Documented in config.hpp: max_inflight_out = 0 forbids QoS > 0 entirely.
    // This profile also sets max_persisted_msg_size = 0, which is what reclaims
    // the buffer on the slot the zero-length-array residue leaves behind.
    CHECK(r.client.publish(etl::string_view("t"), etl::string_view("x"), QoS::AtLeastOnce) ==
          Error::NotSupported);
    CHECK(r.client.publish(etl::string_view("t"), etl::string_view("x"), QoS::ExactlyOnce) ==
          Error::NotSupported);

    // QoS 0 still works, and takes no slot.
    CHECK(r.client.publish(etl::string_view("t"), etl::string_view("x")) == Error::Ok);
    CHECK_EQ(r.client.inflight_count(), size_t{0});
}

TEST(config_with_a_tiny_transmit_queue_reports_it_rather_than_overrunning)
{
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    // 32 bytes of queue. Nothing drains while the transport refuses writes, so
    // publishing repeatedly must eventually be refused -- cleanly.
    r.transport.send_blocked = true;

    bool refused = false;
    for (int i = 0; i < 20 && !refused; ++i)
    {
        const Error e =
            r.client.publish(etl::string_view("topic/x"), etl::string_view("0123456789"));
        if (e != Error::Ok)
        {
            CHECK(e == Error::TxQueueFull || e == Error::BufferTooSmall);
            refused = true;
        }
    }
    CHECK(refused);
    CHECK(r.client.is_connected());   // a full queue is not a protocol failure

    // And it recovers once the transport drains.
    r.transport.send_blocked = false;
    CHECK(r.client.step() == Error::Ok);
    CHECK(r.client.publish(etl::string_view("topic/x"), etl::string_view("ok")) == Error::Ok);
}

TEST(config_with_a_packet_larger_than_the_queue_reports_buffer_too_small)
{
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    // A payload that cannot fit in a 32-byte queue however empty it is. This is
    // the size check before the reserve, not back-pressure.
    static const uint8_t big[64] = {};
    CHECK(r.client.publish(etl::string_view("t"), etl::span<const uint8_t>(big, sizeof(big))) ==
          Error::BufferTooSmall);
    CHECK(r.client.is_connected());
}

TEST(config_with_a_tiny_receive_buffer_ends_the_session_on_an_oversized_packet)
{
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    // Announce a packet larger than the 64-byte receive buffer. There is
    // nowhere to put it and no way to skip it, so the session has to end.
    const uint8_t oversized_header[] = {0x30, 0xC8, 0x01};   // PUBLISH, 200 bytes
    r.transport.push_inbound(oversized_header, sizeof(oversized_header));

    CHECK(r.client.step() == Error::PacketTooLarge);
    CHECK(r.client.state() == State::Idle);
}

TEST(config_with_retransmission_disabled_holds_until_the_reconnect)
{
    Rig<NoRetryConfig> r;
    // Keep-alive off and a resumable session: the point is that the *retry
    // timer* never fires, so nothing else may end the session first.
    REQUIRE(r.bring_up(/*keep_alive_s=*/0, /*clean_session=*/false));

    uint16_t id = 0;
    CHECK(r.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::AtLeastOnce,
                           false, &id) == Error::Ok);
    r.client.step();
    r.transport.clear_sent();

    // However long we wait, nothing is re-sent.
    for (int i = 0; i < 5; ++i)
    {
        r.clock.advance(60u * 1000u);
        r.client.step();
    }
    CHECK_EQ(sim::count_sent(r.transport, PacketType::Publish), size_t{0});
    CHECK_EQ(r.client.inflight_count(), size_t{1});   // still owed

    // A reconnect flushes it, with DUP set.
    r.client.abort();
    CHECK_EQ(r.client.inflight_count(), size_t{1});   // survived, not a clean session

    ConnectOptions o;
    o.client_id     = etl::string_view("c");
    o.keep_alive_s  = 0;
    o.clean_session = false;   // keep the inflight message
    r.transport.clear_sent();
    CHECK(r.client.connect(o) == Error::Ok);
    r.client.step();
    sim::push_connack(r.transport, true);
    r.client.step();

    const sim::SentPacket p = sim::find_sent(r.transport, PacketType::Publish);
    REQUIRE(p.valid);
    CHECK(p.header.dup);
}

TEST(config_with_single_slot_tables_still_completes_a_session)
{
    // Every table is one deep. The point is that the client works at the limit,
    // not just comfortably inside it.
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    uint16_t id = 0;
    CHECK(r.client.subscribe(etl::string_view("a/+"), QoS::AtMostOnce,
                             Client<TinyConfig>::MessageHandler(), &id) == Error::Ok);
    r.client.step();

    // One pending ack slot, so a second request has nowhere to go.
    CHECK(r.client.subscribe(etl::string_view("b/+")) == Error::NoPendingAckSlot);

    const uint8_t granted[] = {0x00};
    sim::push_suback(r.transport, id, granted, 1);
    r.client.step();
    CHECK_EQ(r.client.subscription_count(), size_t{1});

    // One subscription slot, so a second filter is refused even once the ack
    // slot is free again.
    CHECK(r.client.subscribe(etl::string_view("b/+")) == Error::NoSubscriptionSlot);

    sim::push_publish(r.transport, "a/x", "hi");
    CHECK(r.client.step() == Error::Ok);
    CHECK(r.client.is_connected());
}

TEST(config_with_one_inbound_slot_holds_back_a_second_qos2_message)
{
    Rig<TinyConfig> r;
    REQUIRE(r.bring_up());

    // max_inflight_in is 1. The first QoS 2 message is tracked and acknowledged.
    sim::push_publish(r.transport, "a/x", "one", QoS::ExactlyOnce, 11);
    CHECK(r.client.step() == Error::Ok);
    CHECK_EQ(sim::count_sent(r.transport, PacketType::Pubrec), size_t{1});

    // The second has nowhere to be recorded, so it is left unacknowledged for
    // the broker to retransmit rather than acknowledged and dropped.
    sim::push_publish(r.transport, "a/y", "two", QoS::ExactlyOnce, 12);
    CHECK(r.client.step() == Error::Ok);
    CHECK_EQ(sim::count_sent(r.transport, PacketType::Pubrec), size_t{1});
    CHECK_EQ(r.client.inbound_overflow_count(), 1u);
    CHECK(r.client.is_connected());

    // Releasing the first frees the slot, and the retransmission is accepted.
    sim::push_ack(r.transport, PacketType::Pubrel, 11);
    r.client.step();
    sim::push_publish(r.transport, "a/y", "two", QoS::ExactlyOnce, 12, /*dup=*/true);
    CHECK(r.client.step() == Error::Ok);
    CHECK_EQ(sim::count_sent(r.transport, PacketType::Pubrec), size_t{2});
}

namespace {

/// One capacity zeroed each, because the guards run in a fixed order and two
/// zeroes at once would only ever report the first.
struct ZeroPendingAcks : DefaultConfig
{
    static constexpr size_t max_pending_acks = 0;
};

struct ZeroInflightIn : DefaultConfig
{
    static constexpr size_t max_inflight_in = 0;
};

struct ZeroSubscriptions : DefaultConfig
{
    static constexpr size_t max_subscriptions = 0;
};

/// Bring a client up far enough to exercise a capacity guard.
template <typename Cfg>
bool connect_client(Client<Cfg>& client, fakes::FakeTransport& transport) noexcept
{
    ConnectOptions opts;
    opts.client_id     = etl::string_view("cid");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;

    if (client.connect(opts) != Error::Ok)
        return false;

    client.step();
    sim::push_connack(transport, false);
    client.step();
    return client.is_connected();
}

}   // namespace

TEST(a_capacity_configured_to_zero_means_none)
{
    // Every table is rounded up by detail::at_least_one so a zero-sized array
    // is never declared. A guard written against the storage rather than
    // against the configured limit therefore answers for one slot when the
    // config asked for none, and `= 0` neither forbids the operation nor
    // reports it -- it just quietly behaves like `= 1`.
    //
    // All four zero-able capacities are checked here because two of them read
    // the configured value and two read the storage, and nothing but a test
    // distinguishes the two.

    // max_inflight_out: documented as forbidding QoS > 0 entirely.
    {
        fakes::FakeTransport transport;
        fakes::FakeClock     clock;
        Client<TinyConfig>   client{transport, clock};
        REQUIRE(connect_client(client, transport));

        CHECK(client.publish(etl::string_view("a"), etl::string_view("x"), QoS::AtLeastOnce) ==
              Error::NotSupported);
    }

    // max_subscriptions: nothing can be subscribed.
    {
        fakes::FakeTransport      transport;
        fakes::FakeClock          clock;
        Client<ZeroSubscriptions> client{transport, clock};
        REQUIRE(connect_client(client, transport));

        CHECK(client.subscribe(etl::string_view("a/b"), QoS::AtMostOnce) ==
              Error::NoSubscriptionSlot);
    }

    // max_pending_acks: no SUBSCRIBE or UNSUBSCRIBE can be tracked, so neither
    // is accepted. This one read the storage and answered Ok.
    {
        fakes::FakeTransport    transport;
        fakes::FakeClock        clock;
        Client<ZeroPendingAcks> client{transport, clock};
        REQUIRE(connect_client(client, transport));

        CHECK(client.subscribe(etl::string_view("a/b"), QoS::AtMostOnce) ==
              Error::NoPendingAckSlot);
        CHECK(client.unsubscribe(etl::string_view("a/b")) == Error::NoPendingAckSlot);
    }

    // max_inflight_in: an inbound QoS 2 message cannot be tracked, so it is
    // counted as an overflow and left unacknowledged for the broker to
    // retransmit. This one read the storage and tracked a message anyway.
    {
        fakes::FakeTransport   transport;
        fakes::FakeClock       clock;
        Client<ZeroInflightIn> client{transport, clock};
        REQUIRE(connect_client(client, transport));

        sim::push_publish(transport, "a/b", "x", QoS::ExactlyOnce, 7);
        client.step();

        CHECK(client.inbound_overflow_count() == 1u);
    }
}
