// Fuzzes the receive path: arbitrary bytes arriving as if from a broker.
//
// This is the library's real attack surface. Everything a peer controls
// reaches the client as a byte stream, and the client must parse it without
// reading out of bounds, looping forever, or allocating -- from any input, not
// only from packets a well-behaved broker would send.
//
// It drives Client::step() rather than the decoders directly, so the fuzzer
// also reaches reassembly across recv() boundaries, the state machine's
// reaction to a packet arriving in the wrong state, and the teardown paths. A
// decoder tested in isolation cannot reach any of those.
//
// The input is split into chunks at pseudo-random boundaries taken from the
// input itself, because a packet split across two recv() calls exercises
// different code from the same packet delivered whole.
//
// Build:  cmake -S . -B build-fuzz -DMQTT_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
// Run:    ./build-fuzz/tests/fuzz_client -max_total_time=60 tests/fuzz/corpus/client
//
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "broker_sim.hpp"
#include "fakes.hpp"
#include "mqtt/client.hpp"

namespace {

struct FuzzConfig : mqtt::DefaultConfig
{
    // Small enough that a fuzzer reaches the capacity-exhaustion paths --
    // full subscription table, no free inflight slot, oversized packet -- in
    // inputs short enough to be worth generating.
    static constexpr size_t rx_buffer_size         = 256;
    static constexpr size_t tx_buffer_size         = 256;
    static constexpr size_t max_topic_len          = 32;
    static constexpr size_t max_inflight_out       = 2;
    static constexpr size_t max_persisted_msg_size = 64;
    static constexpr size_t max_inflight_in        = 2;
    static constexpr size_t max_subscriptions      = 2;
    static constexpr size_t max_pending_acks       = 2;
};

using FuzzClient = mqtt::Client<FuzzConfig>;

/// The client the handlers act on, and what they should do when they run.
///
/// Handlers that do nothing leave a whole class of defect unreachable. A
/// handler runs inside step(), part-way through draining the receive buffer and
/// part-way through operations holding pointers into the client's own tables --
/// so what a handler does to the client is an input, not a constant. Three
/// memory-safety defects in 1.0.0-rc1 lived exactly there, and a fuzzer with
/// inert handlers could not reach any of them however long it ran.
///
/// Only the actions the library documents as allowed are exercised.
/// subscribe() and unsubscribe() from a handler are documented as forbidden --
/// they mutate the subscription table mid-iteration -- so fuzzing them would be
/// generating undefined behaviour rather than finding it.
mqtt::Client<FuzzConfig>* g_client = nullptr;
uint8_t                   g_action = 0;

void act() noexcept
{
    if (g_client == nullptr)
        return;

    switch (g_action & 0x07u)
    {
        case 1: g_client->abort(); break;
        case 2: (void)g_client->disconnect(); break;
        case 3:
            (void)g_client->publish(etl::string_view("f/x"), etl::string_view("y"),
                                    mqtt::QoS::AtMostOnce);
            break;
        case 4:
            // Documented as refused rather than recursive. Calling it is how that
            // stays true.
            (void)g_client->step();
            break;
        case 5:
            (void)g_client->inflight_count();
            (void)g_client->subscription_count();
            (void)g_client->tx_pending();
            break;
        default: break;
    }
}

/// Handlers must outlive the client, so they are file-scope rather than
/// temporaries -- the same rule the library documents for any caller.
auto on_message    = [](const mqtt::Message&) noexcept { act(); };
auto on_connect    = [](const mqtt::ConnackInfo&) noexcept { act(); };
auto on_disconnect = [](mqtt::Error) noexcept { act(); };
auto on_delivery   = [](uint16_t) noexcept { act(); };
auto on_suback     = [](uint16_t, etl::span<const uint8_t>) noexcept { act(); };

}   // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fakes::FakeTransport transport;
    fakes::FakeClock     clock;
    FuzzClient           client{transport, clock};

    g_client = &client;
    g_action = (size > 1) ? data[1] : 0u;

    client.on_message(on_message);
    client.on_connect(on_connect);
    client.on_disconnect(on_disconnect);
    client.on_delivery_complete(on_delivery);
    client.on_suback(on_suback);

    mqtt::ConnectOptions opts;
    opts.client_id     = etl::string_view("fuzz");
    opts.keep_alive_s  = 10;
    opts.clean_session = true;

    if (client.connect(opts) != mqtt::Error::Ok)
        return 0;

    client.step();   // transport connect, CONNECT written

    // Let the first byte decide whether the session is established the normal
    // way or whether the fuzzer's own bytes have to serve as the CONNACK. The
    // second reaches the handshake's rejection paths; the first reaches
    // everything that can only happen once connected.
    size_t pos = 0;
    if (size > 0 && (data[0] & 1u) != 0)
    {
        sim::push_connack(transport, false);
        client.step();
        pos = 1;
    }

    // Subscribing gives inbound PUBLISH somewhere to be delivered, so the
    // dispatch path is reachable rather than dead.
    if (client.is_connected())
        client.subscribe(etl::string_view("f/#"), mqtt::QoS::ExactlyOnce);

    while (pos < size)
    {
        // A chunk length taken from the data itself, so the split points are
        // part of what the fuzzer explores. 1..64 bytes.
        const size_t chunk = static_cast<size_t>(data[pos] & 0x3Fu) + 1u;
        ++pos;

        const size_t take = (chunk < size - pos) ? chunk : (size - pos);
        if (take == 0)
            break;

        transport.push_inbound(data + pos, take);
        pos += take;

        client.step();

        // Advance the clock so keep-alive, the connect timeout and
        // retransmission are all reachable within one input.
        clock.advance(1000);
        client.step();
    }

    client.disconnect();
    client.step();

    // The client is about to go out of scope; the handlers must not outlive it.
    g_client = nullptr;
    return 0;
}
