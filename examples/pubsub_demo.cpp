// SPDX-License-Identifier: MIT
//
// Host smoke test: connect to a broker, subscribe, publish, echo what arrives.
//
//   ./pubsub_demo [host] [port] [topic]
//   ./pubsub_demo test.mosquitto.org 1883 demo/mqtt-embedded
//
// The whole client lives in .bss here, exactly as it would on a target.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mqtt/client.hpp"
#include "posix_transport.hpp"

namespace {

struct DemoConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 2048;
    static constexpr size_t tx_buffer_size         = 2048;
    static constexpr size_t max_topic_len          = 96;
    static constexpr size_t max_client_id_len      = 32;
    static constexpr size_t max_inflight_out       = 4;
    static constexpr size_t max_persisted_msg_size = 256;
    static constexpr size_t max_subscriptions      = 4;
};

// Statically allocated, like it would be on the target.
example::PosixClock g_clock;
char                g_topic[128] = "demo/mqtt-embedded";
volatile bool       g_running    = true;

// CI runs this as an interop test, so the demo has to be able to fail. Every
// stage records what it achieved and main() checks the tally at the end;
// exiting 0 after silently never reaching the broker would make the job green
// for a client that does not work.
int         g_echoes     = 0;   ///< our own publishes, received back
int         g_deliveries = 0;   ///< QoS 1/2 handshakes the client completed
bool        g_connected  = false;
mqtt::Error g_end_reason = mqtt::Error::Ok;

void print_message(const mqtt::Message& m) noexcept
{
    std::printf("  <- [%.*s] qos%d%s %.*s\n", static_cast<int>(m.topic.size()), m.topic.data(),
                static_cast<int>(m.qos), m.retain ? " (retained)" : "",
                static_cast<int>(m.payload.size()),
                reinterpret_cast<const char*>(m.payload.data()));
}

/// Report one expectation. Returns 0 when met, 1 when not, so main can sum.
int expect(bool ok, const char* what) noexcept
{
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    return ok ? 0 : 1;
}

}   // namespace

int main(int argc, char** argv)
{
    const char*    host = (argc > 1) ? argv[1] : "test.mosquitto.org";
    const uint16_t port = static_cast<uint16_t>((argc > 2) ? std::atoi(argv[2]) : 1883);
    if (argc > 3)
        std::snprintf(g_topic, sizeof(g_topic), "%s", argv[3]);

    static example::PosixTransport  transport(host, port);
    static mqtt::Client<DemoConfig> client{transport, g_clock};

    std::printf("mqtt-embedded demo -> %s:%u, topic '%s'\n", host, port, g_topic);
    std::printf("client footprint: %zu bytes\n\n", sizeof(client));

    auto on_connect = [](const mqtt::ConnackInfo& info) {
        std::printf("connected (session_present=%d)\n", info.session_present ? 1 : 0);
        g_connected = true;
    };
    auto on_disconnect = [](mqtt::Error e) {
        std::printf("disconnected: %s\n", mqtt::to_string(e));
        g_end_reason = e;
        g_running    = false;
    };
    auto on_message = [](const mqtt::Message& m) {
        print_message(m);
        ++g_echoes;
    };
    auto on_delivery = [](uint16_t id) {
        std::printf("  delivery complete, id %u\n", id);
        ++g_deliveries;
    };

    client.on_connect(on_connect);
    client.on_disconnect(on_disconnect);
    client.on_message(on_message);
    client.on_delivery_complete(on_delivery);

    mqtt::ConnectOptions opts;
    opts.client_id = etl::string_view("mqtt-embedded-demo");
    // Short enough that the idle stretch below actually provokes a PINGREQ.
    // Keep-alive is measured from the last byte *sent*, so a demo that publishes
    // every second would never ping however long it ran.
    opts.keep_alive_s  = 5;
    opts.clean_session = true;

    if (client.connect(opts) != mqtt::Error::Ok)
    {
        std::printf("connect() rejected the options\n");
        return 1;
    }

    bool     subscribed = false;
    int      published  = 0;
    uint32_t last_pub   = g_clock.now_ms();

    while (g_running && published < 5)
    {
        const mqtt::Error rc = client.step();
        if (rc != mqtt::Error::Ok)
            break;

        if (client.is_connected() && !subscribed)
        {
            const mqtt::Error e =
                client.subscribe(etl::string_view(g_topic), mqtt::QoS::AtLeastOnce);
            if (e == mqtt::Error::Ok)
            {
                std::printf("subscribed to '%s'\n", g_topic);
                subscribed = true;
            }
        }

        // Publish once a second; we should see each message come straight back
        // through our own subscription.
        if (client.is_connected() && subscribed &&
            mqtt::elapsed_ms(g_clock.now_ms(), last_pub) >= 1000)
        {
            char      payload[64];
            const int len = std::snprintf(payload, sizeof(payload),
                                          "hello #%d from mqtt-embedded", published);

            const mqtt::QoS   qos = static_cast<mqtt::QoS>(published % 3);
            const mqtt::Error e   = client.publish(
                etl::string_view(g_topic),
                etl::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload),
                                           static_cast<size_t>(len)),
                qos);

            if (e == mqtt::Error::Ok)
            {
                std::printf("  -> qos%d %s\n", static_cast<int>(qos), payload);
                ++published;
                last_pub = g_clock.now_ms();
            }
            else
            {
                std::printf("  publish deferred: %s\n", mqtt::to_string(e));
                last_pub = g_clock.now_ms();
            }
        }

        transport.wait_readable(50);
    }

    // Drain the last acknowledgements before going away.
    for (int i = 0; i < 40 && client.inflight_count() > 0; ++i)
    {
        client.step();
        transport.wait_readable(50);
    }

    // Then sit quiet for longer than the broker's grace window, which is 1.5x
    // the keep-alive interval. Surviving that with no application traffic is
    // only possible if PINGREQ went out and PINGRESP came back, so the check
    // afterwards tests the behaviour rather than the broker's choice of words
    // in its log -- which varies by version and is not ours to depend on.
    const uint32_t idle_ms = (static_cast<uint32_t>(opts.keep_alive_s) * 1000u * 2u);
    std::printf("idling %.1fs to exercise keep-alive...\n",
                static_cast<double>(idle_ms) / 1000.0);

    const uint32_t idle_started = g_clock.now_ms();
    while (g_running && mqtt::elapsed_ms(g_clock.now_ms(), idle_started) < idle_ms)
    {
        if (client.step() != mqtt::Error::Ok)
            break;
        transport.wait_readable(100);
    }
    const bool survived_idle = client.is_connected();

    client.disconnect();
    for (int i = 0; i < 20 && client.state() != mqtt::State::Idle; ++i)
    {
        client.step();
        transport.wait_readable(10);
    }
    const bool closed_cleanly = (client.state() == mqtt::State::Idle);

    std::printf("\nresults\n");
    int failures = 0;
    failures += expect(g_connected, "broker accepted CONNECT");
    failures += expect(subscribed, "SUBSCRIBE acknowledged");
    failures += expect(published == 5, "published 5 messages at QoS 0, 1 and 2");
    failures += expect(g_deliveries >= 3, "QoS 1/2 handshakes completed");
    failures += expect(g_echoes >= 5, "all 5 messages came back through the subscription");
    failures += expect(survived_idle, "connection held open by keep-alive alone");
    failures += expect(closed_cleanly, "DISCONNECT completed and socket closed");
    failures += expect(g_end_reason == mqtt::Error::Ok, "session ended without an error");

    if (failures != 0)
    {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("\ndone\n");
    return 0;
}
