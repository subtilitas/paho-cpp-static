#include "test_harness.hpp"

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

namespace {

struct TestConfig : DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 512;
    static constexpr size_t   tx_buffer_size         = 512;
    static constexpr size_t   max_topic_len          = 32;
    static constexpr size_t   max_inflight_out       = 2;
    static constexpr size_t   max_persisted_msg_size = 64;
    static constexpr size_t   max_inflight_in        = 2;
    static constexpr size_t   max_subscriptions      = 4;
    static constexpr size_t   max_pending_acks       = 2;
    static constexpr size_t   max_topics_per_request = 2;
    static constexpr uint32_t retry_interval_ms      = 1000;
    static constexpr uint32_t connect_timeout_ms     = 5000;
};

using TestClient = Client<TestConfig>;

/// Transport, clock and client wired together, with a helper that walks the
/// handshake so individual tests can start from "connected".
struct Fixture
{
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    TestClient           client{transport, clock};

    ConnectOptions default_options() const noexcept
    {
        ConnectOptions opts;
        opts.client_id     = etl::string_view("cid");
        opts.keep_alive_s  = 10;
        opts.clean_session = true;
        return opts;
    }

    bool bring_up(bool session_present = false) noexcept
    {
        if (client.connect(default_options()) != Error::Ok)
            return false;
        client.step();   // transport connect + CONNECT out
        sim::push_connack(transport, session_present);
        client.step();   // consume CONNACK
        return client.is_connected();
    }
};

}   // namespace

//------------------------------------------------------------------------------
// Handshake
//------------------------------------------------------------------------------

TEST(client_completes_connect_handshake)
{
    Fixture f;
    int     connects = 0;
    auto    on_conn  = [&](const ConnackInfo&) { ++connects; };
    f.client.on_connect(on_conn);

    CHECK(f.client.state() == State::Idle);
    CHECK(f.client.connect(f.default_options()) == Error::Ok);
    CHECK(f.client.state() == State::Connecting);

    f.client.step();
    CHECK(f.client.state() == State::AwaitingConnack);
    CHECK(sim::find_sent(f.transport, PacketType::Connect).valid);

    sim::push_connack(f.transport, false);
    f.client.step();

    CHECK(f.client.is_connected());
    CHECK_EQ(connects, 1);
    CHECK(f.client.connack().code == ConnackCode::Accepted);
}

TEST(client_waits_for_slow_transport_connect)
{
    Fixture f;
    f.transport.connect_delay = 3;

    CHECK(f.client.connect(f.default_options()) == Error::Ok);

    f.client.step();
    CHECK(f.client.state() == State::Connecting);
    CHECK_EQ(f.transport.sent.size(), size_t{0});   // nothing sent before connect

    f.client.step();
    f.client.step();
    f.client.step();
    CHECK(f.client.state() == State::AwaitingConnack);
    CHECK(sim::find_sent(f.transport, PacketType::Connect).valid);
}

TEST(client_reports_refused_connection)
{
    Fixture f;
    Error   reason = Error::Ok;
    auto    on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    f.client.connect(f.default_options());
    f.client.step();
    sim::push_connack(f.transport, false, ConnackCode::NotAuthorized);
    f.client.step();

    CHECK(reason == Error::ConnectionRefused);
    CHECK(f.client.state() == State::Idle);
    CHECK(f.client.connack().code == ConnackCode::NotAuthorized);
    CHECK_EQ(f.transport.close_calls, 1);
}

TEST(client_times_out_a_stalled_handshake)
{
    Fixture f;
    Error   reason = Error::Ok;
    auto    on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    f.client.connect(f.default_options());
    f.client.step();
    CHECK(f.client.state() == State::AwaitingConnack);

    f.clock.advance(TestConfig::connect_timeout_ms + 1);
    f.client.step();

    CHECK(reason == Error::ConnectTimeout);
    CHECK(f.client.state() == State::Idle);
}

TEST(client_rejects_anonymous_id_on_a_resumable_session)
{
    // MQTT-3.1.3-8: an empty client id asks the broker to assign one, which it
    // can only do for a session it is not expected to remember.
    Fixture        f;
    ConnectOptions opts = f.default_options();
    opts.client_id      = etl::string_view("");

    opts.clean_session = false;
    CHECK(f.client.connect(opts) == Error::InvalidArgument);

    opts.clean_session = true;
    CHECK(f.client.connect(opts) == Error::Ok);
}

TEST(client_rejects_second_connect_while_active)
{
    Fixture f;
    CHECK(f.client.connect(f.default_options()) == Error::Ok);
    CHECK(f.client.connect(f.default_options()) == Error::AlreadyConnected);
}

TEST(client_rejects_oversized_client_id)
{
    Fixture        f;
    ConnectOptions opts = f.default_options();
    opts.client_id      = etl::string_view("this-client-id-is-far-too-long-for-the-config");
    CHECK(f.client.connect(opts) == Error::InvalidArgument);
}

//------------------------------------------------------------------------------
// Outbound publish
//------------------------------------------------------------------------------

TEST(client_publishes_qos0_without_taking_a_slot)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    CHECK(f.client.publish(etl::string_view("a/b"), etl::string_view("hi")) == Error::Ok);
    f.client.step();

    const sim::SentPacket p = sim::find_sent(f.transport, PacketType::Publish);
    REQUIRE(p.valid);
    CHECK(p.header.qos == QoS::AtMostOnce);
    CHECK_EQ(f.client.inflight_count(), size_t{0});
}

TEST(client_completes_qos1_handshake)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    uint16_t delivered = 0;
    auto     on_done   = [&](uint16_t id) { delivered = id; };
    f.client.on_delivery_complete(on_done);

    uint16_t id = 0;
    CHECK(f.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::AtLeastOnce,
                           false, &id) == Error::Ok);
    CHECK(id != 0);
    CHECK_EQ(f.client.inflight_count(), size_t{1});

    f.client.step();
    const sim::SentPacket p = sim::find_sent(f.transport, PacketType::Publish);
    REQUIRE(p.valid);
    CHECK(p.header.qos == QoS::AtLeastOnce);
    CHECK(!p.header.dup);

    sim::push_ack(f.transport, PacketType::Puback, id);
    f.client.step();

    CHECK_EQ(f.client.inflight_count(), size_t{0});
    CHECK_EQ(delivered, id);
}

TEST(client_completes_qos2_handshake)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    uint16_t delivered = 0;
    auto     on_done   = [&](uint16_t x) { delivered = x; };
    f.client.on_delivery_complete(on_done);

    uint16_t id = 0;
    CHECK(f.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::ExactlyOnce,
                           false, &id) == Error::Ok);
    f.client.step();
    CHECK(sim::find_sent(f.transport, PacketType::Publish).valid);

    // PUBREC must produce a PUBREL, and the message must still be inflight.
    sim::push_ack(f.transport, PacketType::Pubrec, id);
    f.client.step();

    const sim::SentPacket rel = sim::find_sent(f.transport, PacketType::Pubrel);
    REQUIRE(rel.valid);
    CHECK_EQ(sim::sent_ack_id(rel), id);
    CHECK_EQ(f.client.inflight_count(), size_t{1});
    CHECK_EQ(delivered, uint16_t{0});

    sim::push_ack(f.transport, PacketType::Pubcomp, id);
    f.client.step();

    CHECK_EQ(f.client.inflight_count(), size_t{0});
    CHECK_EQ(delivered, id);
}

TEST(client_reports_full_inflight_window)
{
    Fixture f;
    REQUIRE(f.bring_up());

    CHECK(f.client.publish(etl::string_view("a"), etl::string_view("1"), QoS::AtLeastOnce) ==
          Error::Ok);
    CHECK(f.client.publish(etl::string_view("b"), etl::string_view("2"), QoS::AtLeastOnce) ==
          Error::Ok);
    // max_inflight_out is 2.
    CHECK(f.client.publish(etl::string_view("c"), etl::string_view("3"), QoS::AtLeastOnce) ==
          Error::NoInflightSlot);
    CHECK_EQ(f.client.inflight_count(), size_t{2});
}

TEST(client_rejects_payload_larger_than_persisted_slot)
{
    Fixture f;
    REQUIRE(f.bring_up());

    // max_persisted_msg_size is 64; this cannot be stored for retransmission.
    uint8_t big[100] = {};
    CHECK(f.client.publish(etl::string_view("a/b"), etl::span<const uint8_t>(big, sizeof(big)),
                           QoS::AtLeastOnce) == Error::PayloadTooLarge);
    CHECK_EQ(f.client.inflight_count(), size_t{0});

    // The same payload is fine at QoS 0, which needs no retransmission copy.
    CHECK(f.client.publish(etl::string_view("a/b"), etl::span<const uint8_t>(big, sizeof(big)),
                           QoS::AtMostOnce) == Error::Ok);
}

TEST(client_rejects_publish_when_not_connected)
{
    Fixture f;
    CHECK(f.client.publish(etl::string_view("a"), etl::string_view("x")) ==
          Error::NotConnected);
}

TEST(client_rejects_wildcards_in_publish_topic)
{
    Fixture f;
    REQUIRE(f.bring_up());
    CHECK(f.client.publish(etl::string_view("a/+"), etl::string_view("x")) ==
          Error::InvalidArgument);
    CHECK(f.client.publish(etl::string_view(""), etl::string_view("x")) ==
          Error::InvalidArgument);
}

TEST(client_retransmits_unacked_publish_with_dup)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    uint16_t id = 0;
    CHECK(f.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::AtLeastOnce,
                           false, &id) == Error::Ok);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Publish), size_t{1});

    f.clock.advance(TestConfig::retry_interval_ms + 1);
    f.client.step();

    CHECK_EQ(sim::count_sent(f.transport, PacketType::Publish), size_t{2});
    const sim::SentPacket second = sim::find_sent(f.transport, PacketType::Publish, 1);
    REQUIRE(second.valid);
    CHECK(second.header.dup);   // the retransmission is flagged, the original is not
}

//------------------------------------------------------------------------------
// Inbound publish
//------------------------------------------------------------------------------

TEST(client_delivers_qos0_message)
{
    Fixture f;
    REQUIRE(f.bring_up());

    int  count            = 0;
    char topic_seen[32]   = {};
    char payload_seen[32] = {};
    auto on_msg           = [&](const Message& m) {
        ++count;
        std::memcpy(topic_seen, m.topic.data(), m.topic.size());
        std::memcpy(payload_seen, m.payload.data(), m.payload.size());
    };
    f.client.on_message(on_msg);

    sim::push_publish(f.transport, "sensors/temp", "21.5");
    f.client.step();

    CHECK_EQ(count, 1);
    CHECK(std::strcmp(topic_seen, "sensors/temp") == 0);
    CHECK(std::strcmp(payload_seen, "21.5") == 0);
}

TEST(client_acks_inbound_qos1)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    int  count  = 0;
    auto on_msg = [&](const Message&) { ++count; };
    f.client.on_message(on_msg);

    sim::push_publish(f.transport, "t", "p", QoS::AtLeastOnce, 42);
    f.client.step();

    CHECK_EQ(count, 1);
    const sim::SentPacket ack = sim::find_sent(f.transport, PacketType::Puback);
    REQUIRE(ack.valid);
    CHECK_EQ(sim::sent_ack_id(ack), uint16_t{42});
}

TEST(client_deduplicates_inbound_qos2)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    int  count  = 0;
    auto on_msg = [&](const Message&) { ++count; };
    f.client.on_message(on_msg);

    sim::push_publish(f.transport, "t", "p", QoS::ExactlyOnce, 7);
    f.client.step();
    CHECK_EQ(count, 1);
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pubrec), size_t{1});

    // A duplicate before PUBREL must be acknowledged again but not re-delivered.
    sim::push_publish(f.transport, "t", "p", QoS::ExactlyOnce, 7, /*dup=*/true);
    f.client.step();
    CHECK_EQ(count, 1);
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pubrec), size_t{2});

    // PUBREL releases the id and must draw a PUBCOMP.
    sim::push_ack(f.transport, PacketType::Pubrel, 7);
    f.client.step();
    const sim::SentPacket comp = sim::find_sent(f.transport, PacketType::Pubcomp);
    REQUIRE(comp.valid);
    CHECK_EQ(sim::sent_ack_id(comp), uint16_t{7});

    // After release the same id is a genuinely new message.
    sim::push_publish(f.transport, "t", "p", QoS::ExactlyOnce, 7);
    f.client.step();
    CHECK_EQ(count, 2);
}

TEST(client_holds_back_qos2_when_tracking_table_is_full)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    int  count  = 0;
    auto on_msg = [&](const Message&) { ++count; };
    f.client.on_message(on_msg);

    // max_inflight_in is 2.
    sim::push_publish(f.transport, "t", "a", QoS::ExactlyOnce, 1);
    sim::push_publish(f.transport, "t", "b", QoS::ExactlyOnce, 2);
    sim::push_publish(f.transport, "t", "c", QoS::ExactlyOnce, 3);
    f.client.step();

    CHECK_EQ(count, 2);
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pubrec), size_t{2});
    CHECK_EQ(f.client.inbound_overflow_count(), uint32_t{1});
    // Critically, the connection survives: an overloaded table is back-pressure,
    // not a protocol error.
    CHECK(f.client.is_connected());
}

//------------------------------------------------------------------------------
// Subscriptions
//------------------------------------------------------------------------------

TEST(client_subscribes_and_routes_to_handler)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    int  specific = 0;
    int  fallback = 0;
    auto on_sub   = [&](const Message&) { ++specific; };
    auto on_any   = [&](const Message&) { ++fallback; };
    f.client.on_message(on_any);

    uint16_t id = 0;
    CHECK(f.client.subscribe(etl::string_view("sensors/+/temp"), QoS::AtLeastOnce,
                             TestClient::MessageHandler(on_sub), &id) == Error::Ok);
    f.client.step();

    const sim::SentPacket sub = sim::find_sent(f.transport, PacketType::Subscribe);
    REQUIRE(sub.valid);
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    const uint8_t granted[] = {0x01};
    sim::push_suback(f.transport, id, granted, 1);
    f.client.step();

    sim::push_publish(f.transport, "sensors/kitchen/temp", "20");
    f.client.step();
    CHECK_EQ(specific, 1);
    CHECK_EQ(fallback, 0);

    // A topic no filter covers falls through to the general handler.
    sim::push_publish(f.transport, "other/thing", "x");
    f.client.step();
    CHECK_EQ(specific, 1);
    CHECK_EQ(fallback, 1);
}

TEST(client_drops_subscription_the_broker_refused)
{
    Fixture f;
    REQUIRE(f.bring_up());

    uint16_t id = 0;
    CHECK(f.client.subscribe(etl::string_view("a/b"), QoS::AtMostOnce,
                             TestClient::MessageHandler(), &id) == Error::Ok);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    const uint8_t refused[] = {kSubackFailure};
    sim::push_suback(f.transport, id, refused, 1);
    f.client.step();

    CHECK_EQ(f.client.subscription_count(), size_t{0});
}

TEST(client_unsubscribes_on_unsuback)
{
    Fixture f;
    REQUIRE(f.bring_up());

    uint16_t sub_id = 0;
    f.client.subscribe(etl::string_view("a/b"), QoS::AtMostOnce, TestClient::MessageHandler(),
                       &sub_id);
    f.client.step();
    const uint8_t granted[] = {0x00};
    sim::push_suback(f.transport, sub_id, granted, 1);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    f.transport.clear_sent();
    uint16_t unsub_id = 0;
    CHECK(f.client.unsubscribe(etl::string_view("a/b"), &unsub_id) == Error::Ok);
    f.client.step();
    CHECK(sim::find_sent(f.transport, PacketType::Unsubscribe).valid);

    sim::push_unsuback(f.transport, unsub_id);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{0});
}

// Two SUBSCRIBEs are outstanding and the broker refuses one filter from each.
// Erasing the first refusal compacts the subscription table, which used to
// invalidate the positions recorded by the second request: the client then
// erased a filter the broker had granted and kept the one it had refused.
TEST(client_survives_a_refusal_that_compacts_the_table)
{
    Fixture f;
    REQUIRE(f.bring_up());

    int  hits_cd = 0;
    auto h_ab    = [](const Message&) {};
    auto h_cd    = [&](const Message&) { ++hits_cd; };

    const TopicSubscription ab[2] = {{etl::string_view("a"), QoS::AtMostOnce},
                                     {etl::string_view("b"), QoS::AtMostOnce}};
    const TopicSubscription cd[2] = {{etl::string_view("c"), QoS::AtMostOnce},
                                     {etl::string_view("d"), QoS::AtMostOnce}};

    uint16_t id1 = 0, id2 = 0;
    CHECK(f.client.subscribe(etl::span<const TopicSubscription>(ab, 2),
                             TestClient::MessageHandler(h_ab), &id1) == Error::Ok);
    CHECK(f.client.subscribe(etl::span<const TopicSubscription>(cd, 2),
                             TestClient::MessageHandler(h_cd), &id2) == Error::Ok);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{4});

    const uint8_t refuse_first[2] = {kSubackFailure, 0x00};
    sim::push_suback(f.transport, id1, refuse_first, 2);   // "a" refused
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{3});

    sim::push_suback(f.transport, id2, refuse_first, 2);   // "c" refused
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{2});

    // "d" was granted, so its handler must still be wired up.
    sim::push_publish(f.transport, "d", "x");
    f.client.step();
    CHECK_EQ(hits_cd, 1);

    // ...and "c" was refused, so nothing must be left claiming it.
    sim::push_publish(f.transport, "c", "x");
    f.client.step();
    CHECK_EQ(hits_cd, 1);
}

// UNSUBACK used to erase by recorded table position, back to front, which only
// works when the caller happens to name filters in ascending table order.
TEST(client_unsubscribes_regardless_of_argument_order)
{
    Fixture f;
    REQUIRE(f.bring_up());

    const uint8_t granted[] = {0x00};
    const char*   names[]   = {"a", "c"};
    for (size_t i = 0; i < 2; ++i)
    {
        uint16_t id = 0;
        f.client.subscribe(etl::string_view(names[i]), QoS::AtMostOnce,
                           TestClient::MessageHandler(), &id);
        f.client.step();
        sim::push_suback(f.transport, id, granted, 1);
        f.client.step();
    }
    CHECK_EQ(f.client.subscription_count(), size_t{2});

    // Name the later entry first.
    const etl::string_view filters[2] = {etl::string_view("c"), etl::string_view("a")};
    uint16_t               uid        = 0;
    CHECK(f.client.unsubscribe(etl::span<const etl::string_view>(filters, 2), &uid) ==
          Error::Ok);
    f.client.step();
    sim::push_unsuback(f.transport, uid);
    f.client.step();

    CHECK_EQ(f.client.subscription_count(), size_t{0});
}

TEST(client_rejects_invalid_filter)
{
    Fixture f;
    REQUIRE(f.bring_up());
    CHECK(f.client.subscribe(etl::string_view("a/#/b")) == Error::InvalidArgument);
    CHECK(f.client.subscribe(etl::string_view("")) == Error::InvalidArgument);
}

TEST(client_reports_full_subscription_table)
{
    Fixture f;
    REQUIRE(f.bring_up());

    // max_subscriptions is 4, max_pending_acks is 2, so ack the requests as we
    // go to keep pending slots free.
    const char* filters[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; ++i)
    {
        uint16_t id = 0;
        CHECK(f.client.subscribe(etl::string_view(filters[i]), QoS::AtMostOnce,
                                 TestClient::MessageHandler(), &id) == Error::Ok);
        f.client.step();
        const uint8_t granted[] = {0x00};
        sim::push_suback(f.transport, id, granted, 1);
        f.client.step();
    }
    CHECK_EQ(f.client.subscription_count(), size_t{4});
    CHECK(f.client.subscribe(etl::string_view("e")) == Error::NoSubscriptionSlot);
}

TEST(client_resubscribes_when_broker_lost_the_session)
{
    Fixture f;
    REQUIRE(f.bring_up());

    uint16_t id = 0;
    f.client.subscribe(etl::string_view("a/b"), QoS::AtLeastOnce, TestClient::MessageHandler(),
                       &id);
    f.client.step();
    const uint8_t granted[] = {0x01};
    sim::push_suback(f.transport, id, granted, 1);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    // Reconnect with a session the broker does not recognise.
    ConnectOptions opts = f.default_options();
    opts.clean_session  = false;
    f.client.abort();
    f.transport.clear_sent();
    CHECK(f.client.connect(opts) == Error::Ok);
    f.client.step();
    sim::push_connack(f.transport, /*session_present=*/false);
    f.client.step();
    REQUIRE(f.client.is_connected());

    // The client must re-send the subscription unprompted.
    f.client.step();
    CHECK(sim::find_sent(f.transport, PacketType::Subscribe).valid);
}

// clean_session tells the broker to discard *its* state. Our own record of what
// the application asked for has to survive, because it is the only thing that
// can rebuild the session -- we never keep the caller's strings alive.
TEST(client_resubscribes_after_a_clean_session_reconnect)
{
    Fixture f;
    REQUIRE(f.bring_up());   // clean_session = true

    uint16_t id = 0;
    f.client.subscribe(etl::string_view("sensors/#"), QoS::AtLeastOnce,
                       TestClient::MessageHandler(), &id);
    f.client.step();
    const uint8_t granted[] = {0x01};
    sim::push_suback(f.transport, id, granted, 1);
    f.client.step();
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    f.client.abort();
    CHECK_EQ(f.client.subscription_count(), size_t{1});

    f.transport.clear_sent();
    REQUIRE(f.bring_up());
    f.client.step();

    CHECK_EQ(f.client.subscription_count(), size_t{1});
    CHECK(sim::find_sent(f.transport, PacketType::Subscribe).valid);
}

//------------------------------------------------------------------------------
// Keep-alive
//------------------------------------------------------------------------------

TEST(client_pings_before_the_keep_alive_expires)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    // keep_alive is 10s; the client pings at 75% of that.
    f.clock.advance(7000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{0});

    f.clock.advance(1000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{1});

    // One outstanding ping at a time.
    f.clock.advance(1000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{1});

    sim::push_pingresp(f.transport);
    f.client.step();
    f.clock.advance(8000);
    f.client.step();
    CHECK_EQ(sim::count_sent(f.transport, PacketType::Pingreq), size_t{2});
}

TEST(client_drops_connection_when_pingresp_never_arrives)
{
    Fixture f;
    REQUIRE(f.bring_up());

    Error reason = Error::Ok;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    f.clock.advance(8000);
    f.client.step();   // PINGREQ goes out

    f.clock.advance(10001);   // one full keep-alive with no PINGRESP
    f.client.step();

    CHECK(reason == Error::KeepAliveTimeout);
    CHECK(f.client.state() == State::Idle);
}

//------------------------------------------------------------------------------
// Transport behaviour
//------------------------------------------------------------------------------

TEST(client_survives_byte_at_a_time_transport)
{
    Fixture f;
    f.transport.send_limit = 1;
    f.transport.recv_limit = 1;

    CHECK(f.client.connect(f.default_options()) == Error::Ok);

    // Drain the CONNECT one byte per send().
    for (int i = 0; i < 64 && f.client.tx_pending() > 0; ++i)
        f.client.step();
    CHECK_EQ(f.client.tx_pending(), size_t{0});
    CHECK(sim::find_sent(f.transport, PacketType::Connect).valid);

    sim::push_connack(f.transport, false);
    for (int i = 0; i < 16 && !f.client.is_connected(); ++i)
        f.client.step();
    CHECK(f.client.is_connected());

    // And a fragmented inbound PUBLISH must still reassemble.
    int  count  = 0;
    auto on_msg = [&](const Message& m) {
        if (m.topic == etl::string_view("frag/topic"))
            ++count;
    };
    f.client.on_message(on_msg);

    sim::push_publish(f.transport, "frag/topic", "some payload here");
    for (int i = 0; i < 64 && count == 0; ++i)
        f.client.step();
    CHECK_EQ(count, 1);
}

TEST(client_holds_data_while_transport_refuses_writes)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();
    f.transport.send_blocked = true;

    CHECK(f.client.publish(etl::string_view("a/b"), etl::string_view("hi")) == Error::Ok);
    f.client.step();
    CHECK(f.client.tx_pending() > 0);
    CHECK_EQ(f.transport.sent.size(), size_t{0});
    CHECK(f.client.is_connected());   // back-pressure is not an error

    f.transport.send_blocked = false;
    f.client.step();
    CHECK_EQ(f.client.tx_pending(), size_t{0});
    CHECK(sim::find_sent(f.transport, PacketType::Publish).valid);
}

// A transmit queue with no room is back-pressure, not a protocol error. The
// acknowledgement is deferred, the message is not delivered, and the broker's
// own retransmission closes the gap -- the session must stay up.
TEST(client_defers_acks_instead_of_dropping_the_session)
{
    Fixture f;
    REQUIRE(f.bring_up());

    // topic "t" plus 250 payload bytes serializes to 256; twice fills the
    // 512-byte queue exactly, leaving no room for a four-byte PUBACK.
    f.transport.send_blocked = true;
    uint8_t filler[250]      = {};
    for (int i = 0; i < 2; ++i)
        CHECK(f.client.publish(etl::string_view("t"),
                               etl::span<const uint8_t>(filler, sizeof(filler))) == Error::Ok);
    CHECK_EQ(f.client.tx_pending(), size_t{512});

    int  delivered = 0;
    auto sink      = [&](const Message&) { ++delivered; };
    f.client.on_message(sink);

    sim::push_publish(f.transport, "t", "x", QoS::AtLeastOnce, 7);
    CHECK(f.client.step() == Error::Ok);

    CHECK(f.client.is_connected());
    CHECK_EQ(f.client.tx_backpressure_count(), 1u);
    CHECK_EQ(delivered, 0);   // not acked, so not delivered either

    // Once the queue drains, the broker's retransmission is accepted normally.
    f.transport.send_blocked = false;
    f.client.step();
    f.transport.clear_sent();

    sim::push_publish(f.transport, "t", "x", QoS::AtLeastOnce, 7, /*dup=*/true);
    f.client.step();

    CHECK_EQ(delivered, 1);
    const sim::SentPacket ack = sim::find_sent(f.transport, PacketType::Puback);
    REQUIRE(ack.valid);
    CHECK_EQ(sim::sent_ack_id(ack), uint16_t{7});
}

TEST(client_reports_transport_close)
{
    Fixture f;
    REQUIRE(f.bring_up());

    Error reason = Error::Ok;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    f.transport.close_when_drained = true;
    f.client.step();

    CHECK(reason == Error::TransportClosed);
    CHECK(f.client.state() == State::Idle);
}

TEST(client_rejects_malformed_inbound_packet)
{
    Fixture f;
    REQUIRE(f.bring_up());

    Error reason = Error::Ok;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    const uint8_t garbage[] = {0x00, 0x00};   // reserved packet type
    f.transport.push_inbound(garbage, sizeof(garbage));
    f.client.step();

    CHECK(reason == Error::MalformedPacket);
    CHECK(f.client.state() == State::Idle);
}

TEST(client_rejects_packet_larger_than_receive_buffer)
{
    Fixture f;
    REQUIRE(f.bring_up());

    Error reason = Error::Ok;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    // A PUBLISH claiming 100000 bytes cannot fit in a 512-byte buffer.
    const uint8_t oversized[] = {0x30, 0xA0, 0x8D, 0x06};
    f.transport.push_inbound(oversized, sizeof(oversized));
    f.client.step();

    CHECK(reason == Error::PacketTooLarge);
    CHECK(f.client.state() == State::Idle);
}

TEST(client_rejects_packets_a_server_must_not_send)
{
    Fixture f;
    REQUIRE(f.bring_up());

    Error reason = Error::Ok;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    const uint8_t pingreq[] = {0xC0, 0x00};   // clients send these, servers do not
    f.transport.push_inbound(pingreq, sizeof(pingreq));
    f.client.step();

    CHECK(reason == Error::ProtocolViolation);
}

TEST(client_handles_several_packets_in_one_read)
{
    Fixture f;
    REQUIRE(f.bring_up());

    int  count  = 0;
    auto on_msg = [&](const Message&) { ++count; };
    f.client.on_message(on_msg);

    sim::push_publish(f.transport, "a", "1");
    sim::push_publish(f.transport, "b", "2");
    sim::push_publish(f.transport, "c", "3");
    f.client.step();

    CHECK_EQ(count, 3);
}

//------------------------------------------------------------------------------
// Shutdown
//------------------------------------------------------------------------------

TEST(client_sends_disconnect_then_closes)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    Error reason = Error::TransportFailure;
    auto  on_dis = [&](Error e) { reason = e; };
    f.client.on_disconnect(on_dis);

    CHECK(f.client.disconnect() == Error::Ok);
    CHECK(f.client.state() == State::Disconnecting);

    f.client.step();

    CHECK(sim::find_sent(f.transport, PacketType::Disconnect).valid);
    CHECK(f.client.state() == State::Idle);
    CHECK(reason == Error::Ok);
    CHECK(!f.transport.is_connected());
}

TEST(client_abort_skips_the_disconnect_packet)
{
    Fixture f;
    REQUIRE(f.bring_up());
    f.transport.clear_sent();

    f.client.abort();

    CHECK_EQ(sim::count_sent(f.transport, PacketType::Disconnect), size_t{0});
    CHECK(f.client.state() == State::Idle);
    CHECK_EQ(f.transport.close_calls, 1);
}

TEST(client_keeps_inflight_across_a_non_clean_reconnect)
{
    Fixture f;

    ConnectOptions opts = f.default_options();
    opts.clean_session  = false;

    CHECK(f.client.connect(opts) == Error::Ok);
    f.client.step();
    sim::push_connack(f.transport, true);
    f.client.step();
    REQUIRE(f.client.is_connected());

    uint16_t id = 0;
    f.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::AtLeastOnce, false,
                     &id);
    f.client.step();
    CHECK_EQ(f.client.inflight_count(), size_t{1});

    f.client.abort();
    CHECK_EQ(f.client.inflight_count(), size_t{1});   // still owed to the broker

    f.transport.clear_sent();
    CHECK(f.client.connect(opts) == Error::Ok);
    f.client.step();
    sim::push_connack(f.transport, true);
    f.client.step();
    f.clock.advance(TestConfig::retry_interval_ms + 1);
    f.client.step();

    const sim::SentPacket p = sim::find_sent(f.transport, PacketType::Publish);
    REQUIRE(p.valid);
    CHECK(p.header.dup);
}

TEST(client_discards_inflight_on_a_clean_reconnect)
{
    Fixture f;
    REQUIRE(f.bring_up());   // clean_session = true

    f.client.publish(etl::string_view("a/b"), etl::string_view("hi"), QoS::AtLeastOnce);
    CHECK_EQ(f.client.inflight_count(), size_t{1});

    f.client.abort();
    CHECK_EQ(f.client.inflight_count(), size_t{0});
}
