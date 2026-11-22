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

#include <iostream>

#include <etl/string.h>
#include <etl/to_string.h>

#include "cli.hpp"
#include "mqtt/client.hpp"

// The library has no OS dependency, so a port is a transport and a clock and
// nothing else. This demo picks the pair for the platform and is otherwise
// identical on both -- which is the claim, demonstrated rather than asserted.
#if defined(_WIN32)
#include "win_clock.hpp"
#include "winsock_transport.hpp"
#else
#include "posix_clock.hpp"
#include "tcp_ip_transport.hpp"
#endif

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

etl::string<NodeConfig::max_topic_len> g_topic("demo/ip");
bool                                   g_running = true;

}   // namespace

int main(int argc, char** argv)
{
    example::Ipv4 broker = kDefaultBroker;
    if (argc > 1 && !example::parse_ipv4(argv[1], broker))
    {
        std::cerr << "'" << argv[1] << "' is not a dotted-quad IPv4 address\n";
        return 2;
    }

    uint16_t port = 1883;
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

#if defined(_WIN32)
    static example::WinClock         clock;
    static example::WinsockTransport transport(broker, port);
#else
    static example::PosixClock     clock;
    static example::TcpIpTransport transport(broker, port);
#endif
    static mqtt::Client<NodeConfig> client{transport, clock};

    std::cout << "connecting to " << static_cast<unsigned>(broker.octets[0]) << '.'
              << static_cast<unsigned>(broker.octets[1]) << '.'
              << static_cast<unsigned>(broker.octets[2]) << '.'
              << static_cast<unsigned>(broker.octets[3]) << ':' << port << ", topic '"
              << example::Text{g_topic} << "'\n";
    std::cout << "client footprint: " << sizeof(client) << " bytes\n\n";

    static auto on_connect = [](const mqtt::ConnackInfo& info) {
        std::cout << "connected (session_present=" << (info.session_present ? 1 : 0) << ")\n";
    };
    static auto on_disconnect = [](mqtt::Error e) {
        std::cout << "disconnected: " << mqtt::to_string(e) << '\n';
        g_running = false;
    };
    static auto on_message = [](const mqtt::Message& m) {
        const etl::string_view payload(reinterpret_cast<const char*>(m.payload.data()),
                                       m.payload.size());
        std::cout << "  <- [" << example::Text{m.topic} << "] " << example::Text{payload}
                  << '\n';
    };
    static auto on_delivery = [](uint16_t id) {
        std::cout << "  delivered, id " << id << '\n';
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
        std::cerr << "connect() rejected the options\n";
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
            client.subscribe(etl::string_view(g_topic.data(), g_topic.size()),
                             mqtt::QoS::AtLeastOnce) == mqtt::Error::Ok)
        {
            std::cout << "subscribed to '" << example::Text{g_topic} << "'\n";
            subscribed = true;
        }

        if (subscribed && mqtt::elapsed_ms(clock.now_ms(), last_pub) >= 1000)
        {
            // "reading " and at most 11 characters of an int.
            etl::string<24> payload("reading ");
            (void)etl::to_string(sent, payload, true);

            if (client.publish(
                    etl::string_view(g_topic.data(), g_topic.size()),
                    etl::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()),
                                             payload.size()),
                    mqtt::QoS::AtLeastOnce) == mqtt::Error::Ok)
            {
                std::cout << "  -> " << example::Text{payload} << '\n';
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

    std::cout << "done\n";
    return 0;
}
