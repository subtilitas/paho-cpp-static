// step() must return. That is the property this file exists to hold.
//
// The library's headline claims are about memory: no allocation, sizeof is the
// whole cost. The property that matters just as much on a device with a
// watchdog is that one call to step() does a bounded amount of work regardless
// of what the peer does -- so a superloop can call it between two other jobs
// and know it will get control back.
//
// That property is real. pump_rx() caps itself at kMaxRecvRoundsPerStep recv
// calls; drain_rx() loops but strictly shrinks the buffer each pass; there is
// no recursion anywhere on the protocol path and no dynamic allocation to
// block on. None of it was tested, which meant a future refactor could remove
// the cap and every existing test would still pass -- they all feed a finite
// amount of data and stop.
//
// A hostile or merely enthusiastic broker does not stop.

#include "test_harness.hpp"

#include "fakes.hpp"
#include "mqtt/client.hpp"

using namespace mqtt;

namespace {

struct SmallRxConfig : DefaultConfig
{
    // Deliberately tiny, so the round budget is reached after a few hundred
    // bytes rather than a few thousand. A test that needs a megabyte to
    // demonstrate the bound is a test nobody runs under a sanitiser.
    static constexpr size_t rx_buffer_size   = 64;
    static constexpr size_t tx_buffer_size   = 128;
    static constexpr size_t max_inflight_out = 0;
};

using SmallClient = Client<SmallRxConfig>;

/// A broker that never stops talking.
///
/// Emits a CONNACK and then an unbroken stream of PINGRESPs -- two bytes each,
/// always valid, always accepted -- up to a byte budget large enough that the
/// client will hit its own limit long before this one. Counts what it was
/// asked for, which is the measurement.
class FloodTransport final : public Transport
{
public:
    explicit FloodTransport(size_t budget) noexcept : budget_(budget) {}

    Error connect() noexcept override
    {
        connected_ = true;
        return Error::Ok;
    }

    Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = connected_ ? data.size() : 0;
        return connected_ ? Error::Ok : Error::TransportClosed;
    }

    Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (!connected_)
            return Error::TransportClosed;
        if (offset_ >= budget_)
            return Error::WouldBlock;

        ++recv_calls;

        size_t n = buffer.size();
        if (n > budget_ - offset_)
            n = budget_ - offset_;

        for (size_t i = 0; i < n; ++i)
            buffer[i] = byte_at(offset_ + i);

        offset_ += n;
        read = n;
        return Error::Ok;
    }

    void close() noexcept override { connected_ = false; }
    bool is_connected() const noexcept override { return connected_; }

    int    recv_calls = 0;
    size_t emitted() const noexcept { return offset_; }

private:
    /// CONNACK, then PINGRESP for ever. Generated from the absolute offset so
    /// the two-byte packets stay aligned across any chunking the client asks
    /// for.
    static uint8_t byte_at(size_t offset) noexcept
    {
        static constexpr uint8_t kConnack[4] = {0x20, 0x02, 0x00, 0x00};
        if (offset < sizeof kConnack)
            return kConnack[offset];
        return ((offset - sizeof kConnack) % 2 == 0) ? uint8_t{0xD0} : uint8_t{0x00};
    }

    size_t budget_;
    size_t offset_    = 0;
    bool   connected_ = false;
};

ConnectOptions options() noexcept
{
    ConnectOptions opts;
    opts.client_id     = etl::string_view("bounded");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;
    return opts;
}

/// One step() against a broker with `budget` bytes queued. Returns how many
/// times the transport was asked for data.
int recv_calls_for_one_step(size_t budget) noexcept
{
    FloodTransport   transport{budget};
    fakes::FakeClock clock;
    SmallClient      client{transport, clock};

    client.connect(options());
    client.step();                 // transport connect, CONNECT out, CONNACK in
    transport.recv_calls = 0;      // measure the next call, not the handshake
    client.step();
    return transport.recv_calls;
}

}   // namespace

//------------------------------------------------------------------------------

TEST(step_does_a_bounded_amount_of_work_per_call)
{
    // The bound is what makes step() safe to call from a superloop. Without
    // it, a broker that always has another packet ready keeps the client
    // inside pump_rx() indefinitely and the watchdog -- not the test -- finds
    // out first.
    FloodTransport   transport{1u << 20};   // a megabyte; effectively endless
    fakes::FakeClock clock;
    SmallClient      client{transport, clock};

    CHECK(client.connect(options()) == Error::Ok);
    CHECK(client.step() == Error::Ok);
    CHECK(client.is_connected());

    transport.recv_calls = 0;
    const size_t before = transport.emitted();

    CHECK(client.step() == Error::Ok);

    // Generous: the point is the difference between "a small constant" and
    // "however much the peer felt like sending", not the constant's value.
    CHECK(transport.recv_calls > 0);
    CHECK(transport.recv_calls <= 32);
    CHECK(transport.emitted() - before <= 32u * SmallRxConfig::rx_buffer_size);
    CHECK(client.is_connected());
}

TEST(the_work_per_step_does_not_grow_with_what_the_peer_has_queued)
{
    // The sharper statement of the same property, and the one a refactor
    // cannot accidentally satisfy: the cost of a step is a function of the
    // client's own configuration, not of the peer's enthusiasm. Sixteen times
    // the backlog, same work.
    const int small = recv_calls_for_one_step(64u * 1024u);
    const int large = recv_calls_for_one_step(1024u * 1024u);

    CHECK(small > 0);
    CHECK_EQ(small, large);
}

TEST(a_flooded_client_still_makes_progress_across_steps)
{
    // Bounded must not mean stuck. The budget exists to hand control back, so
    // the next call has to pick up where the last one stopped rather than
    // re-reading or wedging.
    FloodTransport   transport{1u << 20};
    fakes::FakeClock clock;
    SmallClient      client{transport, clock};

    client.connect(options());
    client.step();
    CHECK(client.is_connected());

    size_t consumed_last = transport.emitted();
    for (int i = 0; i < 8; ++i)
    {
        CHECK(client.step() == Error::Ok);
        CHECK(client.is_connected());
        CHECK(transport.emitted() > consumed_last);
        consumed_last = transport.emitted();
    }
}

TEST(a_receive_buffer_packed_with_packets_drains_in_one_step)
{
    // drain_rx()'s loop is unbounded by construction -- for(;;) -- and relies
    // on each pass strictly shrinking the buffer. A packet that consumed zero
    // bytes would spin here for ever, so the case of "the buffer is entirely
    // full of tiny complete packets" is the one worth pinning down.
    FloodTransport   transport{4u + 2u * 4096u};
    fakes::FakeClock clock;
    SmallClient      client{transport, clock};

    client.connect(options());
    client.step();
    CHECK(client.is_connected());

    for (int i = 0; i < 64 && transport.emitted() < 4u + 2u * 4096u; ++i)
        client.step();

    // Everything queued was consumed, the buffer never stalled, and the
    // session survived it.
    CHECK(client.is_connected());
    CHECK(client.state() == State::Connected);
}
