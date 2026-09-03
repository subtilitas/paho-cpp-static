// What a handler is allowed to do to the client that is calling it.
//
// Handlers run inside step(), part-way through draining the receive buffer and
// part-way through operations that hold pointers into the client's tables. A
// handler that ends the session pulls that state out from under the frame that
// invoked it.
//
// Three defects of this shape were live in 1.0.0-rc1, and every case below
// failed under AddressSanitizer or UndefinedBehaviorSanitizer before the fix:
//
//   * drain_rx measured the packet size before the callback and subtracted it
//     from rx_len_ afterwards. abort() sets rx_len_ to zero, so the size_t
//     subtraction wrapped and memmove was handed a length near SIZE_MAX.
//   * handle_suback invoked the callback and then erased the pending-ack entry
//     through a pointer that shutdown() had already invalidated.
//   * step() called from a handler re-drained the same bytes, re-entered the
//     same handler, and recursed until the stack ran out.
//
// None of them is reachable without a handler that acts on the client, which is
// why the rest of the suite -- and the fuzz targets, whose handlers did nothing
// -- did not reach them.

#include "test_harness.hpp"

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

namespace {

struct ReentrancyConfig : DefaultConfig
{
    static constexpr size_t rx_buffer_size    = 512;
    static constexpr size_t tx_buffer_size    = 512;
    static constexpr size_t max_subscriptions = 4;
};

using ReClient = Client<ReentrancyConfig>;

/// What the handler under test should do when it runs.
enum class Act
{
    Nothing,
    Abort,
    Disconnect,
    Step,
};

Act   g_act        = Act::Nothing;
int   g_calls      = 0;
Error g_step_error = Error::Ok;

/// The client the handlers act on. A pointer rather than a reference because
/// each case builds its own fixture.
///
/// It names a Fixture member, so it outlives nothing: the fixture sets it on
/// construction and clears it on destruction, and only one fixture is alive at
/// a time. act() checks it anyway rather than resting on that -- a handler that
/// ran with no fixture would be a null dereference, and the check costs one
/// comparison in a test.
ReClient* g_client = nullptr;

void act() noexcept
{
    ++g_calls;

    if (g_client == nullptr)
        return;

    switch (g_act)
    {
        case Act::Abort: g_client->abort(); break;
        case Act::Disconnect: (void)g_client->disconnect(); break;
        case Act::Step: g_step_error = g_client->step(); break;
        case Act::Nothing: break;
    }
}

// Named, file-scope handlers: a callback slot stores a pointer to the callable,
// so a temporary would not compile.
auto on_msg_act      = [](const Message&) noexcept { act(); };
auto on_connect_act  = [](const ConnackInfo&) noexcept { act(); };
auto on_suback_act   = [](uint16_t, etl::span<const uint8_t>) noexcept { act(); };
auto on_delivery_act = [](uint16_t) noexcept { act(); };

struct Fixture
{
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    ReClient             client{transport, clock};

    Fixture() noexcept
    {
        g_client     = &client;
        g_act        = Act::Nothing;
        g_calls      = 0;
        g_step_error = Error::Ok;
    }

    ~Fixture() noexcept { g_client = nullptr; }

    bool bring_up() noexcept
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

    /// Subscribe and acknowledge, so an inbound PUBLISH has somewhere to go.
    bool subscribe_ok(const char* filter) noexcept
    {
        uint16_t id = 0;
        if (client.subscribe(etl::string_view(filter), QoS::AtMostOnce,
                             ReClient::MessageHandler(), &id) != Error::Ok)
            return false;

        client.step();

        const uint8_t codes[] = {0};
        sim::push_suback(transport, id, codes, 1);
        client.step();
        return true;
    }
};

}   // namespace

TEST(handler_may_abort_from_on_message)
{
    Fixture f;
    f.client.on_message(on_msg_act);
    REQUIRE(f.bring_up());
    REQUIRE(f.subscribe_ok("a/b"));

    g_act = Act::Abort;
    sim::push_publish(f.transport, "a/b", "hello", QoS::AtMostOnce, 0);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() == State::Idle);
    CHECK(f.client.last_error() == Error::TransportClosed);
}

TEST(handler_may_abort_from_on_connect)
{
    Fixture f;
    f.client.on_connect(on_connect_act);

    ConnectOptions opts;
    opts.client_id     = etl::string_view("cid");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;

    REQUIRE(f.client.connect(opts) == Error::Ok);
    f.client.step();

    g_act = Act::Abort;
    sim::push_connack(f.transport, false);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() == State::Idle);
}

TEST(handler_may_abort_from_on_suback)
{
    // The one that erased through a pointer shutdown() had invalidated.
    Fixture f;
    f.client.on_suback(on_suback_act);
    REQUIRE(f.bring_up());

    uint16_t id = 0;
    REQUIRE(f.client.subscribe(etl::string_view("a/b"), QoS::AtMostOnce,
                               ReClient::MessageHandler(), &id) == Error::Ok);
    f.client.step();

    g_act                 = Act::Abort;
    const uint8_t codes[] = {0};
    sim::push_suback(f.transport, id, codes, 1);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() == State::Idle);
}

TEST(handler_may_abort_from_on_delivery_complete)
{
    Fixture f;
    f.client.on_delivery_complete(on_delivery_act);
    REQUIRE(f.bring_up());

    uint16_t id = 0;
    REQUIRE(f.client.publish(etl::string_view("a/b"), etl::string_view("x"), QoS::AtLeastOnce,
                             false, &id) == Error::Ok);
    f.client.step();

    g_act = Act::Abort;
    sim::push_ack(f.transport, PacketType::Puback, id);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() == State::Idle);
}

TEST(handler_may_disconnect_from_on_message)
{
    // disconnect() with a healthy transmit queue does not empty the receive
    // buffer -- it moves to Disconnecting and lets the DISCONNECT go out. It
    // was already safe; pinned so it stays that way.
    Fixture f;
    f.client.on_message(on_msg_act);
    REQUIRE(f.bring_up());
    REQUIRE(f.subscribe_ok("a/b"));

    g_act = Act::Disconnect;
    sim::push_publish(f.transport, "a/b", "hello", QoS::AtMostOnce, 0);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() != State::Connected);
}

TEST(step_from_a_handler_is_refused_rather_than_recursing)
{
    Fixture f;
    f.client.on_message(on_msg_act);
    REQUIRE(f.bring_up());
    REQUIRE(f.subscribe_ok("a/b"));

    g_act = Act::Step;
    sim::push_publish(f.transport, "a/b", "hello", QoS::AtMostOnce, 0);
    f.client.step();

    // Called once, not once per level of a recursion that would not terminate.
    CHECK(g_calls == 1);
    CHECK(g_step_error == Error::Reentrant);
    CHECK(f.client.is_connected());
}

TEST(a_second_packet_after_an_aborting_handler_is_discarded)
{
    // Two packets in the buffer at once: the first handler ends the session, so
    // the second must not be delivered on a connection that no longer exists.
    Fixture f;
    f.client.on_message(on_msg_act);
    REQUIRE(f.bring_up());
    REQUIRE(f.subscribe_ok("a/b"));

    g_act = Act::Abort;
    sim::push_publish(f.transport, "a/b", "one", QoS::AtMostOnce, 0);
    sim::push_publish(f.transport, "a/b", "two", QoS::AtMostOnce, 0);
    f.client.step();

    CHECK(g_calls == 1);
    CHECK(f.client.state() == State::Idle);
}
