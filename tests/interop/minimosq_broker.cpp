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
#include <cstdio>
#include <cstdlib>

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

        char client_buf[128];
        char topic_buf[128];

        std::fprintf(stderr, "minimosq: %-22s client='%s' topic='%s' err=%s\n",
                     minimosq::event_kind_name(e.kind),
                     text(e.client_id, client_buf, sizeof client_buf),
                     text(e.topic, topic_buf, sizeof topic_buf), minimosq::err_name(e.err));
        std::fflush(stderr);
    }

    /// StrView is not NUL-terminated and may hold a null pointer, so it is
    /// copied before printf sees it.
    ///
    /// The buffer belongs to the caller rather than being a static here. Two of
    /// these appear as arguments to one fprintf call, and a shared buffer would
    /// leave both pointing at whichever copy happened to run last -- printing
    /// the topic as the client id, in a log whose only job is to say what went
    /// wrong.
    static const char* text(minimosq::StrView s, char* buf, size_t cap) noexcept
    {
        if (cap == 0)
            return "";

        if (s.data == nullptr || s.len == 0)
        {
            buf[0] = '\0';
            return buf;
        }

        const size_t n = (s.len < cap - 1) ? s.len : cap - 1;
        for (size_t i = 0; i < n; ++i)
            buf[i] = s.data[i];
        buf[n] = '\0';
        return buf;
    }
};

using BrokerTraits = minimosq::DefaultTraits;
using Transport    = minimosq::TcpTransport<BrokerTraits::max_connections>;

Transport transport;
minimosq::Broker<BrokerTraits, Transport, minimosq::AllowAllSecurity, ReportingObserver> broker{
    transport};

void on_signal(int) { transport.stop(); }

/// strtol reports every failure the same way atol does -- which is to say not
/// at all -- so the parse is checked rather than trusted. "1883x" must not
/// start a broker on 1883.
bool parse_port(const char* text, uint16_t& out) noexcept
{
    char*      end   = nullptr;
    const long value = std::strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < 1 || value > 65535)
        return false;

    out = static_cast<uint16_t>(value);
    return true;
}

}   // namespace

int main(int argc, char** argv)
{
    uint16_t port = 1883;
    if (argc > 1 && !parse_port(argv[1], port))
    {
        std::fprintf(stderr, "usage: %s [port]   (1-65535)\n", argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!transport.open(port))
    {
        std::fprintf(stderr, "minimosq_broker: cannot listen on port %u\n", port);
        return 2;
    }

    std::printf("minimosq_broker: MQTT 3.1.1 broker listening on port %u\n", transport.port());
    std::printf("minimosq_broker: broker state is %zu bytes, statically allocated\n",
                sizeof broker);
    std::fflush(stdout);

    transport.run(broker);

    const ReportingObserver& o = broker.observer();

    std::printf("\nminimosq_broker: %lu events\n", o.total_events);
    std::printf("  protocol violations   %lu\n", o.violations);
    std::printf("  connect refusals      %lu\n", o.refusals);
    std::printf("  transport send failed %lu\n", o.send_failed);
    std::printf("  deliveries dropped    %lu\n", o.dropped);
    std::printf("  timeouts              %lu\n", o.timeouts);

    // Timeouts are not counted as failures. The demo client idles deliberately
    // to exercise keep-alive, and a session that ends by the client going away
    // is a normal end to the run.
    const unsigned long fatal = o.violations + o.refusals + o.send_failed + o.dropped;

    if (fatal != 0)
    {
        std::printf(
            "\nminimosq_broker: FAIL -- %lu event(s) the client should not have caused\n",
            fatal);
        return 1;
    }

    std::printf("\nminimosq_broker: ok -- no protocol violations\n");
    return 0;
}
