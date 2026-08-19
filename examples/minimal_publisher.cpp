// SPDX-License-Identifier: MIT
//
// The smallest complete program: connect, publish one QoS 1 message, leave.
//
//   ./minimal_publisher [host] [port]
//
// Everything is static. There is no heap in this program's steady state, and
// the same code shape works unchanged on a microcontroller once you swap the
// transport.

#include <cstdio>
#include <cstdlib>

#include "mqtt/client.hpp"
#include "posix_transport.hpp"

namespace {

struct Config : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 256;
    static constexpr size_t tx_buffer_size         = 256;
    static constexpr size_t max_inflight_out       = 1;
    static constexpr size_t max_persisted_msg_size = 128;
    static constexpr size_t max_subscriptions      = 1;
};

// The entire memory cost of the client, visible at compile time.
static_assert(sizeof(mqtt::Client<Config>) < 1500, "client is larger than budgeted");

bool g_done = false;

}   // namespace

int main(int argc, char** argv)
{
    const char*    host = (argc > 1) ? argv[1] : "127.0.0.1";
    const uint16_t port = static_cast<uint16_t>((argc > 2) ? std::atoi(argv[2]) : 1883);

    static example::PosixClock     clock;
    static example::PosixTransport transport(host, port);
    static mqtt::Client<Config>    client{transport, clock};

    static auto on_delivered = [](uint16_t id) {
        std::printf("delivered, packet id %u\n", id);
        g_done = true;
    };
    client.on_delivery_complete(on_delivered);

    mqtt::ConnectOptions opts;
    opts.client_id    = etl::string_view("minimal-publisher");
    opts.keep_alive_s = 30;

    if (client.connect(opts) != mqtt::Error::Ok)
        return 1;

    bool published = false;

    while (!g_done)
    {
        const mqtt::Error rc = client.step();
        if (rc != mqtt::Error::Ok)
        {
            std::printf("session ended: %s\n", mqtt::to_string(rc));
            return 1;
        }

        if (client.is_connected() && !published)
        {
            const mqtt::Error e =
                client.publish(etl::string_view("demo/minimal"), etl::string_view("hello"),
                               mqtt::QoS::AtLeastOnce);
            if (e == mqtt::Error::Ok)
                published = true;
        }

        transport.wait_readable(50);
    }

    client.disconnect();
    while (client.state() != mqtt::State::Idle)
        client.step();

    return 0;
}
