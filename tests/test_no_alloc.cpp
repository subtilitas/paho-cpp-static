// The load-bearing test.
//
// Replaces global operator new/delete with counting versions and drives a full
// MQTT session -- connect, subscribe, publish at all three QoS levels, inbound
// traffic, keep-alive, retransmission, disconnect -- with the counter armed.
// Any allocation the client performs after construction shows up here.

#include "test_harness.hpp"

#include <cstdlib>
#include <new>

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

//------------------------------------------------------------------------------
// Allocation counters. Replacing the global operator new is program-wide, which
// is exactly the coverage we want: it catches allocations from anywhere in the
// call graph, including inside ETL.
//------------------------------------------------------------------------------

namespace alloc_probe {

volatile bool armed     = false;
size_t        new_calls = 0;
size_t        new_bytes = 0;

void reset() noexcept
{
    new_calls = 0;
    new_bytes = 0;
}

}   // namespace alloc_probe

void* operator new(size_t size)
{
    if (alloc_probe::armed)
    {
        ++alloc_probe::new_calls;
        alloc_probe::new_bytes += size;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr)
        std::abort();   // -fno-exceptions: we cannot throw bad_alloc
    return p;
}

void* operator new[](size_t size) { return operator new(size); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

//------------------------------------------------------------------------------

namespace {

struct ProbeConfig : DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 512;
    static constexpr size_t   tx_buffer_size         = 512;
    static constexpr size_t   max_topic_len          = 32;
    static constexpr size_t   max_inflight_out       = 4;
    static constexpr size_t   max_persisted_msg_size = 64;
    static constexpr size_t   max_inflight_in        = 4;
    static constexpr size_t   max_subscriptions      = 4;
    static constexpr size_t   max_pending_acks       = 4;
    static constexpr size_t   max_topics_per_request = 2;
    static constexpr uint32_t retry_interval_ms      = 1000;
};

using ProbeClient = Client<ProbeConfig>;

struct SmallConfig : DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 256;
    static constexpr size_t tx_buffer_size         = 256;
    static constexpr size_t max_inflight_out       = 1;
    static constexpr size_t max_persisted_msg_size = 64;
    static constexpr size_t max_subscriptions      = 2;
};

struct LargeConfig : DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 4096;
    static constexpr size_t tx_buffer_size         = 4096;
    static constexpr size_t max_inflight_out       = 16;
    static constexpr size_t max_persisted_msg_size = 512;
    static constexpr size_t max_subscriptions      = 32;
};

}   // namespace

TEST(zero_allocations_during_a_full_session)
{
    // Construct everything with the probe disarmed. Construction is the one
    // point where allocation would be permitted by the design contract -- and
    // in fact none of these types allocate there either, which the next test
    // checks separately.
    static fakes::FakeTransport transport;
    static fakes::FakeClock     clock;
    static ProbeClient          client{transport, clock};

    int  messages  = 0;
    int  delivered = 0;
    auto on_msg    = [&](const Message&) { ++messages; };
    auto on_done   = [&](uint16_t) { ++delivered; };
    auto on_conn   = [](const ConnackInfo&) {};
    auto on_dis    = [](Error) {};
    auto on_sub    = [](uint16_t, etl::span<const uint8_t>) {};

    client.on_message(on_msg);
    client.on_delivery_complete(on_done);
    client.on_connect(on_conn);
    client.on_disconnect(on_dis);
    client.on_suback(on_sub);

    // Everything from here on must be allocation-free.
    alloc_probe::reset();
    alloc_probe::armed = true;

    ConnectOptions opts;
    opts.client_id     = etl::string_view("probe-client");
    opts.username      = etl::string_view("user");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;

    CHECK(client.connect(opts) == Error::Ok);
    client.step();
    sim::push_connack(transport, false);
    client.step();
    CHECK(client.is_connected());

    // Subscribe, and take the SUBACK.
    uint16_t sub_id = 0;
    CHECK(client.subscribe(etl::string_view("sensors/+/temp"), QoS::AtLeastOnce,
                           ProbeClient::MessageHandler(on_msg), &sub_id) == Error::Ok);
    client.step();
    const uint8_t granted[] = {0x01};
    sim::push_suback(transport, sub_id, granted, 1);
    client.step();

    // Publish at every QoS level and complete each handshake.
    CHECK(client.publish(etl::string_view("a/b"), etl::string_view("qos0")) == Error::Ok);

    uint16_t id1 = 0;
    CHECK(client.publish(etl::string_view("a/b"), etl::string_view("qos1"), QoS::AtLeastOnce,
                         false, &id1) == Error::Ok);

    uint16_t id2 = 0;
    CHECK(client.publish(etl::string_view("a/b"), etl::string_view("qos2"), QoS::ExactlyOnce,
                         false, &id2) == Error::Ok);
    client.step();

    sim::push_ack(transport, PacketType::Puback, id1);
    sim::push_ack(transport, PacketType::Pubrec, id2);
    client.step();
    sim::push_ack(transport, PacketType::Pubcomp, id2);
    client.step();
    CHECK_EQ(delivered, 2);

    // Inbound traffic at every QoS level.
    sim::push_publish(transport, "sensors/kitchen/temp", "21.5");
    sim::push_publish(transport, "sensors/hall/temp", "19.0", QoS::AtLeastOnce, 100);
    sim::push_publish(transport, "sensors/attic/temp", "30.0", QoS::ExactlyOnce, 101);
    client.step();
    sim::push_ack(transport, PacketType::Pubrel, 101);
    client.step();
    CHECK_EQ(messages, 3);

    // Keep-alive and retransmission paths.
    uint16_t id3 = 0;
    client.publish(etl::string_view("a/b"), etl::string_view("retry"), QoS::AtLeastOnce, false,
                   &id3);
    client.step();
    clock.advance(8000);
    client.step();   // PINGREQ plus a retransmission
    sim::push_pingresp(transport);
    client.step();
    sim::push_ack(transport, PacketType::Puback, id3);
    client.step();

    // Unsubscribe and shut down.
    uint16_t unsub_id = 0;
    CHECK(client.unsubscribe(etl::string_view("sensors/+/temp"), &unsub_id) == Error::Ok);
    client.step();
    sim::push_unsuback(transport, unsub_id);
    client.step();

    CHECK(client.disconnect() == Error::Ok);
    client.step();
    CHECK(client.state() == State::Idle);

    alloc_probe::armed = false;

    if (alloc_probe::new_calls != 0)
    {
        std::printf("    %zu allocations totalling %zu bytes\n", alloc_probe::new_calls,
                    alloc_probe::new_bytes);
    }
    CHECK_EQ(alloc_probe::new_calls, size_t{0});
    CHECK_EQ(alloc_probe::new_bytes, size_t{0});
}

TEST(zero_allocations_during_construction)
{
    // The contract permits allocation at startup, but this implementation does
    // not need any: a Client can live in .bss on a target with no heap linked.
    alloc_probe::reset();
    alloc_probe::armed = true;

    {
        static fakes::FakeTransport transport;
        static fakes::FakeClock     clock;
        static ProbeClient          client{transport, clock};
        CHECK(client.state() == State::Idle);
    }

    alloc_probe::armed = false;
    CHECK_EQ(alloc_probe::new_calls, size_t{0});
}

TEST(zero_allocations_under_error_and_exhaustion_paths)
{
    static fakes::FakeTransport transport;
    static fakes::FakeClock     clock;
    static ProbeClient          client{transport, clock};

    alloc_probe::reset();
    alloc_probe::armed = true;

    ConnectOptions opts;
    opts.client_id = etl::string_view("probe2");
    client.connect(opts);
    client.step();
    sim::push_connack(transport, false);
    client.step();

    // Fill the inflight window, then keep pushing.
    for (int i = 0; i < 10; ++i)
    {
        client.publish(etl::string_view("a/b"), etl::string_view("x"), QoS::AtLeastOnce);
    }

    // Oversized payload, bad topics, bad filters.
    uint8_t big[400] = {};
    client.publish(etl::string_view("a/b"), etl::span<const uint8_t>(big, sizeof(big)),
                   QoS::ExactlyOnce);
    client.publish(etl::string_view("a/#"), etl::string_view("x"));
    client.subscribe(etl::string_view("bad/#/filter"));

    // Fill the subscription and pending-ack tables.
    const char* filters[] = {"f1", "f2", "f3", "f4", "f5", "f6"};
    for (size_t i = 0; i < 6; ++i)
        client.subscribe(etl::string_view(filters[i]));

    client.step();

    // A malformed packet tears the connection down; that path must not allocate.
    const uint8_t garbage[] = {0x00, 0x00};
    transport.push_inbound(garbage, sizeof(garbage));
    client.step();

    alloc_probe::armed = false;
    CHECK_EQ(alloc_probe::new_calls, size_t{0});
}

TEST(client_footprint_is_a_pure_function_of_config)
{
    // Not an assertion about a specific number -- just a demonstration that the
    // whole cost is visible and scales with the config, with nothing hidden
    // behind a pointer.
    std::printf("    sizeof(Client<Small>)   = %zu bytes\n", sizeof(Client<SmallConfig>));
    std::printf("    sizeof(Client<Default>) = %zu bytes\n", sizeof(Client<DefaultConfig>));
    std::printf("    sizeof(Client<Large>)   = %zu bytes\n", sizeof(Client<LargeConfig>));

    CHECK(sizeof(Client<SmallConfig>) < sizeof(Client<DefaultConfig>));
    CHECK(sizeof(Client<DefaultConfig>) < sizeof(Client<LargeConfig>));
}
