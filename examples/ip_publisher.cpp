// SPDX-License-Identifier: MIT
//
// Connect to a broker at a fixed IPv4 address, subscribe, publish, echo.
//
//   ./ip_publisher [address] [port] [topic]
//   ./ip_publisher 192.168.1.50 1883 sensors/node07
//
// The difference from pubsub_demo is the transport: no DNS, no getaddrinfo, no
// resolver. The address is four octets validated at compile time when it is a
// literal. This is the arrangement most shipped devices end up with.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mqtt/client.hpp"
#include "posix_clock.hpp"
#include "tcp_ip_transport.hpp"

namespace {

struct NodeConfig : mqtt::DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 512;
    static constexpr size_t   tx_buffer_size         = 512;
    static constexpr size_t   max_topic_len          = 64;
    static constexpr size_t   max_client_id_len      = 24;
    static constexpr size_t   max_inflight_out       = 4;
    static constexpr size_t   max_persisted_msg_size = 128;
    static constexpr size_t   max_subscriptions      = 4;
    static constexpr uint32_t retry_interval_ms      = 10000;
};

// The parser is constexpr, so a hard-coded broker address is checked at build
// time rather than failing to connect in the field.
constexpr example::Ipv4 kDefaultBroker = example::ipv4("127.0.0.1");
static_assert(kDefaultBroker.octets[0] == 127, "broker address failed to parse");

// And the client's footprint is a compile-time number you can budget against.
static_assert(sizeof(mqtt::Client<NodeConfig>) < 3072, "MQTT client over budget");

char g_topic[80] = "demo/ip";
bool g_running   = true;

} // namespace

int main(int argc, char** argv)
{
    example::Ipv4 broker = kDefaultBroker;
    if (argc > 1 && !example::parse_ipv4(argv[1], broker))
    {
        std::printf("'%s' is not a dotted-quad IPv4 address\n", argv[1]);
        return 2;
    }

    const uint16_t port = static_cast<uint16_t>((argc > 2) ? std::atoi(argv[2]) : 1883);
    if (argc > 3)
        std::snprintf(g_topic, sizeof(g_topic), "%s", argv[3]);

    static example::PosixClock      clock;
    static example::TcpIpTransport  transport(broker, port);
    static mqtt::Client<NodeConfig> client{transport, clock};

    std::printf("connecting to %u.%u.%u.%u:%u, topic '%s'\n",
                broker.octets[0], broker.octets[1], broker.octets[2], broker.octets[3],
                port, g_topic);
    std::printf("client footprint: %zu bytes\n\n", sizeof(client));

    static auto on_connect = [](const mqtt::ConnackInfo& info) {
        std::printf("connected (session_present=%d)\n", info.session_present ? 1 : 0);
    };
    static auto on_disconnect = [](mqtt::Error e) {
        std::printf("disconnected: %s\n", mqtt::to_string(e));
        g_running = false;
    };
    static auto on_message = [](const mqtt::Message& m) {
        std::printf("  <- [%.*s] %.*s\n",
                    static_cast<int>(m.topic.size()), m.topic.data(),
                    static_cast<int>(m.payload.size()),
                    reinterpret_cast<const char*>(m.payload.data()));
    };
    static auto on_delivery = [](uint16_t id) {
        std::printf("  delivered, id %u\n", id);
    };

    client.on_connect(on_connect);
    client.on_disconnect(on_disconnect);
    client.on_message(on_message);
    client.on_delivery_complete(on_delivery);

    mqtt::ConnectOptions opts;
    opts.client_id     = etl::string_view("ip-publisher");
    opts.keep_alive_s  = 20;
    opts.clean_session = true;

    if (client.connect(opts) != mqtt::Error::Ok)
    {
        std::printf("connect() rejected the options\n");
        return 1;
    }

    bool     subscribed = false;
    int      sent       = 0;
    uint32_t last_pub   = clock.now_ms();

    while (g_running && sent < 3)
    {
        if (client.step() != mqtt::Error::Ok)
            break;

        if (client.is_connected() && !subscribed &&
            client.subscribe(etl::string_view(g_topic), mqtt::QoS::AtLeastOnce) ==
                mqtt::Error::Ok)
        {
            std::printf("subscribed to '%s'\n", g_topic);
            subscribed = true;
        }

        if (subscribed && mqtt::elapsed_ms(clock.now_ms(), last_pub) >= 1000)
        {
            char      payload[48];
            const int len = std::snprintf(payload, sizeof(payload), "reading %d", sent);

            if (client.publish(etl::string_view(g_topic),
                               etl::span<const uint8_t>(
                                   reinterpret_cast<const uint8_t*>(payload),
                                   static_cast<size_t>(len)),
                               mqtt::QoS::AtLeastOnce) == mqtt::Error::Ok)
            {
                std::printf("  -> %s\n", payload);
                ++sent;
            }
            last_pub = clock.now_ms();
        }

        transport.wait_readable(50);
    }

    for (int i = 0; i < 40 && client.inflight_count() > 0; ++i)
    {
        client.step();
        transport.wait_readable(50);
    }

    client.disconnect();
    for (int i = 0; i < 10 && client.state() != mqtt::State::Idle; ++i)
        client.step();

    std::printf("done\n");
    return 0;
}
