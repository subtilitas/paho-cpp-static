// SPDX-License-Identifier: MIT
//
// Host smoke test: connect to a broker, subscribe, publish, echo what arrives.
//
//   ./pubsub_demo [host] [port] [topic]
//   ./pubsub_demo test.mosquitto.org 1883 demo/mqtt-embedded
//
// The whole client lives in .bss here, exactly as it would on a target.

#include <iomanip>
#include <iostream>

#include <etl/string.h>
#include <etl/to_string.h>

#include "cli.hpp"
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
example::PosixClock                    g_clock;
etl::string<DemoConfig::max_topic_len> g_topic("demo/mqtt-embedded");
volatile bool                          g_running = true;

/// Upper bound on the whole session. Generous next to the ~16s a healthy run
/// takes, tight enough that a wedged broker fails the job in a minute rather
/// than occupying a CI runner until its limit.
constexpr uint32_t kSessionDeadlineMs = 60000;

// CI runs this as an interop test, so the demo has to be able to fail. Every
// stage records what it achieved and main() checks the tally at the end;
// exiting 0 after silently never reaching the broker would make the job green
// for a client that does not work.
int         g_echoes     = 0;   ///< our own publishes, received back
int         g_deliveries = 0;   ///< QoS 1/2 handshakes the client completed
bool        g_connected  = false;
mqtt::Error g_end_reason = mqtt::Error::Ok;

void print_message(const mqtt::Message& m)
{
    const etl::string_view payload(reinterpret_cast<const char*>(m.payload.data()),
                                   m.payload.size());
    std::cout << "  <- [" << example::Text{m.topic} << "] qos" << static_cast<int>(m.qos)
              << (m.retain ? " (retained)" : "") << ' ' << example::Text{payload} << '\n';
}

/// Report one expectation. Returns 0 when met, 1 when not, so main can sum.
int expect(bool ok, const char* what)
{
    std::cout << "  " << std::left << std::setw(4) << (ok ? "ok" : "FAIL") << std::right << ' '
              << what << '\n';
    return ok ? 0 : 1;
}

}   // namespace

int main(int argc, char** argv)
{
    const char* host = (argc > 1) ? argv[1] : "test.mosquitto.org";
    uint16_t    port = 1883;
    if ((argc > 2) && !example::parse_port(argv[2], port))
    {
        std::cerr << "'" << argv[2] << "' is not a port number from 1 to 65535\n";
        return 2;
    }

    if (argc > 3)
    {
        const etl::string_view topic(argv[3]);
        if (topic.size() > g_topic.max_size())
        {
            std::cerr << "topic is " << topic.size() << " bytes; the limit is "
                      << g_topic.max_size() << '\n';
            return 2;
        }
        g_topic.assign(topic.begin(), topic.end());
    }

    static example::PosixTransport  transport(host, port);
    static mqtt::Client<DemoConfig> client{transport, g_clock};

    std::cout << "mqtt-embedded demo -> " << host << ':' << port << ", topic '"
              << example::Text{g_topic} << "'\n";
    std::cout << "client footprint: " << sizeof(client) << " bytes\n\n";

    auto on_connect = [](const mqtt::ConnackInfo& info) {
        std::cout << "connected (session_present=" << (info.session_present ? 1 : 0) << ")\n";
        g_connected = true;
    };
    auto on_disconnect = [](mqtt::Error e) {
        std::cout << "disconnected: " << mqtt::to_string(e) << '\n';
        g_end_reason = e;
        g_running    = false;
    };
    auto on_message = [](const mqtt::Message& m) {
        print_message(m);
        ++g_echoes;
    };
    auto on_delivery = [](uint16_t id) {
        std::cout << "  delivery complete, id " << id << '\n';
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
        std::cerr << "connect() rejected the options\n";
        return 1;
    }

    bool     subscribed = false;
    int      published  = 0;
    int      deferrals  = 0;
    uint32_t last_pub   = g_clock.now_ms();

    // A deadline on the whole session, because every other stopping condition
    // here depends on making progress. If the broker accepts the connection
    // and then acknowledges nothing, the client stays healthy -- keep-alive
    // still round-trips, so none of its own timeouts fire -- while publish()
    // returns NoInflightSlot forever and `published` never advances. Without
    // this the loop spins until the CI runner's job limit, which is a six-hour
    // way of reporting a failure that is obvious in ten seconds.
    const uint32_t started_ms = g_clock.now_ms();
    bool           timed_out  = false;

    while (g_running && published < 5)
    {
        if (mqtt::elapsed_ms(g_clock.now_ms(), started_ms) > kSessionDeadlineMs)
        {
            std::cout << "\ntimed out after " << (kSessionDeadlineMs / 1000u)
                      << "s: connected=" << (g_connected ? 1 : 0)
                      << " subscribed=" << (subscribed ? 1 : 0) << " published=" << published
                      << '\n';
            timed_out = true;
            break;
        }

        const mqtt::Error rc = client.step();
        if (rc != mqtt::Error::Ok)
            break;

        if (client.is_connected() && !subscribed)
        {
            const mqtt::Error e = client.subscribe(
                etl::string_view(g_topic.data(), g_topic.size()), mqtt::QoS::AtLeastOnce);
            if (e == mqtt::Error::Ok)
            {
                std::cout << "subscribed to '" << example::Text{g_topic} << "'\n";
                subscribed = true;
            }
        }

        // Publish once a second; we should see each message come straight back
        // through our own subscription.
        if (client.is_connected() && subscribed &&
            mqtt::elapsed_ms(g_clock.now_ms(), last_pub) >= 1000)
        {
            // "hello #", at most 11 characters of an int, " from mqtt-embedded".
            etl::string<40> payload("hello #");
            (void)etl::to_string(published, payload, true);
            payload.append(" from mqtt-embedded");

            const mqtt::QoS   qos = static_cast<mqtt::QoS>(published % 3);
            const mqtt::Error e   = client.publish(
                etl::string_view(g_topic.data(), g_topic.size()),
                etl::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()),
                                           payload.size()),
                qos);

            if (e == mqtt::Error::Ok)
            {
                std::cout << "  -> qos" << static_cast<int>(qos) << ' '
                          << example::Text{payload} << '\n';
                ++published;
                deferrals = 0;
                last_pub  = g_clock.now_ms();
            }
            else
            {
                // The QoS is chosen from `published`, so a level that fails
                // persistently is retried at that same level forever. Give up
                // rather than let the deadline above be the only backstop, and
                // say which error it was.
                std::cout << "  publish deferred: " << mqtt::to_string(e) << '\n';
                if (++deferrals >= 10)
                {
                    std::cout << "\ngave up after " << deferrals
                              << " consecutive deferrals at qos" << static_cast<int>(qos)
                              << '\n';
                    timed_out = true;
                    break;
                }
                last_pub = g_clock.now_ms();
            }
        }

        transport.wait_readable(50);
    }

    // Drain the last acknowledgements before going away. Skipped once the
    // deadline has fired: the session is already a failure and there is no
    // sense spending another ten seconds proving it twice.
    for (int i = 0; i < 40 && !timed_out && client.inflight_count() > 0; ++i)
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
    if (!timed_out)
        std::cout << "idling " << (idle_ms / 1000u) << '.' << ((idle_ms % 1000u) / 100u)
                  << "s to exercise keep-alive...\n";

    const uint32_t idle_started = g_clock.now_ms();
    while (!timed_out && g_running &&
           mqtt::elapsed_ms(g_clock.now_ms(), idle_started) < idle_ms)
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

    std::cout << "\nresults\n";
    int failures = 0;
    failures += expect(!timed_out, "session completed without hitting the deadline");
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
        std::cout << "\n" << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "\ndone\n";
    return 0;
}
