// An MQTT 3.1.1 broker for the interop job, built on minimosq.
//
//   ./minimosq_broker [port]        (default 1883)
//
// The Mosquitto interop job asserts by grepping the broker's log for words
// like "protocol error". This one does better, because minimosq reports its
// decisions through an observer policy rather than a log: every protocol
// violation, refusal and dropped delivery arrives here as a typed event. The
// process exits non-zero if any of them happened, so the assertion is
// structural rather than a string match that a wording change would silently
// defeat.
//
// Why a second broker at all: Mosquitto is one implementation, and a client
// tested against exactly one implementation is tested against its leniency as
// much as against the specification. minimosq is an independent implementation
// of the same specification, so the two disagree where this client is wrong.
//
// The minimosq release ships headers only -- no example programs -- so the
// broker's main() lives here. MQTT_MINIMOSQ_DIR points at an unpacked release.
//
// SPDX-License-Identifier: MIT

#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include <etl/string_view.h>
#include <etl/to_arithmetic.h>

#include <minimosq/minimosq.hpp>
#include <minimosq/transports/posix/tcp.hpp>

namespace {

/// Counts the events that mean the client under test did something wrong, and
/// prints every event so a failing run is readable rather than just red.
///
/// on_event() runs inside a broker entry point and must not call back into the
/// broker; printing and incrementing counters is all this does.
struct ReportingObserver
{
    unsigned long violations   = 0;   ///< the peer broke the protocol
    unsigned long refusals     = 0;   ///< CONNACK carried a refusal code
    unsigned long send_failed  = 0;   ///< the transport refused a write
    unsigned long dropped      = 0;   ///< a delivery the broker could not make
    unsigned long timeouts     = 0;   ///< connect, keep-alive or idle expiry
    unsigned long total_events = 0;

    void on_event(const minimosq::Event& e) noexcept
    {
        ++total_events;

        switch (e.kind)
        {
            case minimosq::EventKind::protocol_violation: ++violations; break;
            case minimosq::EventKind::connect_refused: ++refusals; break;
            case minimosq::EventKind::transport_send_failed: ++send_failed; break;
            case minimosq::EventKind::delivery_dropped: ++dropped; break;
            case minimosq::EventKind::connect_timeout:
            case minimosq::EventKind::keepalive_timeout:
            case minimosq::EventKind::idle_timeout: ++timeouts; break;
            default: break;
        }

        // std::cerr is unit-buffered, so each line is out before the next
        // event, and before a crash could lose it.
        std::cerr << "minimosq: " << std::left << std::setw(22)
                  << minimosq::event_kind_name(e.kind) << std::right << " client='"
                  << Text{e.client_id} << "' topic='" << Text{e.topic}
                  << "' err=" << minimosq::err_name(e.err) << '\n';
    }

    /// StrView is not NUL-terminated and may hold a null pointer, so it is
    /// written by length, and not at all when it is null or empty.
    struct Text
    {
        minimosq::StrView view;
    };

    friend std::ostream& operator<<(std::ostream& os, Text t)
    {
        if (t.view.data != nullptr && t.view.len != 0)
            (void)os.write(t.view.data, static_cast<std::streamsize>(t.view.len));
        return os;
    }
};

using BrokerTraits = minimosq::DefaultTraits;
using Transport    = minimosq::TcpTransport<BrokerTraits::max_connections>;

Transport transport;
minimosq::Broker<BrokerTraits, Transport, minimosq::AllowAllSecurity, ReportingObserver> broker{
    transport};

void on_signal(int) { transport.stop(); }

/// Decimal digits only, 1 to 65535. etl::to_arithmetic reports a bad format
/// and an overflow, so "1883x" does not start a broker on 1883 and "70000" does
/// not start one on 4464.
bool parse_port(const char* text, uint16_t& out) noexcept
{
    if (text == nullptr)
        return false;

    const etl::string_view digits(text);
    if (digits.empty() || (digits.front() < '0') || (digits.front() > '9'))
        return false;

    const etl::to_arithmetic_result<uint32_t> parsed = etl::to_arithmetic<uint32_t>(digits);
    if (!parsed.has_value() || parsed.value() < 1u || parsed.value() > 65535u)
        return false;

    out = static_cast<uint16_t>(parsed.value());
    return true;
}

}   // namespace

int main(int argc, char** argv)
{
    uint16_t port = 1883;
    if (argc > 1 && !parse_port(argv[1], port))
    {
        std::cerr << "usage: " << argv[0] << " [port]   (1-65535)\n";
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!transport.open(port))
    {
        std::cerr << "minimosq_broker: cannot listen on port " << port << '\n';
        return 2;
    }

    // Flushed: the interop job polls the log for the first line to know the
    // broker is up.
    std::cout << "minimosq_broker: MQTT 3.1.1 broker listening on port " << transport.port()
              << '\n';
    std::cout << "minimosq_broker: broker state is " << sizeof broker
              << " bytes, statically allocated" << std::endl;

    transport.run(broker);

    const ReportingObserver& o = broker.observer();

    std::cout << "\nminimosq_broker: " << o.total_events << " events\n";
    std::cout << "  protocol violations   " << o.violations << '\n';
    std::cout << "  connect refusals      " << o.refusals << '\n';
    std::cout << "  transport send failed " << o.send_failed << '\n';
    std::cout << "  deliveries dropped    " << o.dropped << '\n';
    std::cout << "  timeouts              " << o.timeouts << '\n';

    // Timeouts are not counted as failures. The demo client idles deliberately
    // to exercise keep-alive, and a session that ends by the client going away
    // is a normal end to the run.
    const unsigned long fatal = o.violations + o.refusals + o.send_failed + o.dropped;

    if (fatal != 0)
    {
        std::cout << "\nminimosq_broker: FAIL -- " << fatal
                  << " event(s) the client should not have caused\n";
        return 1;
    }

    std::cout << "\nminimosq_broker: ok -- no protocol violations\n";
    return 0;
}
