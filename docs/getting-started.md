# Getting started

A working publish and subscribe against a real broker, then the shape of your
own first program.

## Build and run the demo

```bash
git clone https://github.com/subtilitas/paho-cpp-static.git
cd paho-cpp-static
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake fetches the [Embedded Template Library](https://www.etlcpp.com/) for you.
If you already have a checkout, point at it with `-DMQTT_ETL_DIR=/path/to/etl`
and nothing is downloaded.

Then, against a broker on your machine:

```bash
# Debian/Ubuntu: sudo apt-get install mosquitto mosquitto-clients
mosquitto -p 1883 &

./build/examples/ip_publisher 127.0.0.1 1883 demo/hello
```

```
connecting to 127.0.0.1:1883, topic 'demo/hello'
client footprint: 2552 bytes
connected (session_present=0)
subscribed to 'demo/hello'
  -> reading 0
  <- [demo/hello] reading 0
  delivered, id 2
```

The message goes out at QoS 1 and returns through the client's own
subscription, so one run exercises publish, subscribe, delivery and
acknowledgement.

### No broker to hand?

`tools/stub_broker.py` is a small MQTT 3.1.1 broker written against the standard
library, for when installing one is not an option. It needs no arguments and no
dependencies:

```bash
python3 tools/stub_broker.py --port 1883 &

./build/examples/pubsub_demo 127.0.0.1 1883 demo/hello
```

It holds a real session: both directions at QoS 0, 1 and 2 with the full
acknowledgement handshakes, subscribe and unsubscribe, keep-alive, and routing
of published messages back to matching subscriptions. It also provokes failures
a healthy broker will not:

```bash
python3 tools/stub_broker.py --refuse 5        # CONNACK "not authorized"
python3 tools/stub_broker.py --drop-after 8    # socket dies mid-session
python3 tools/stub_broker.py --session-present # exercise session resumption
```

It is permissive, so it will not catch a client sending something the spec
forbids. Check against real Mosquitto before believing a port works, as this
project's CI does on every push.

## The three examples

| Example | Transport | Use it to |
|---|---|---|
| `minimal_publisher` | hostname via `getaddrinfo` | see the smallest complete program |
| `pubsub_demo` | hostname via `getaddrinfo` | watch all three QoS levels in both directions |
| `ip_publisher` | fixed IPv4, no DNS | copy as the starting point for a device |

`ip_publisher` is the one to start from for embedded work — see
[Porting](Porting) for why the resolver is worth losing.

## Your first program

Four things: a config, a transport, a clock, and a loop.

```cpp
#include "mqtt/client.hpp"

// 1. Capacities. Everything is sized from this; nothing grows at run time.
struct MyConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size    = 1024;
    static constexpr size_t max_inflight_out  = 4;
    static constexpr size_t max_subscriptions = 8;
};

// 2 and 3. Yours. See the Porting guide; the examples have working ones.
static MyTransport            transport{"192.168.1.50", 1883};
static MyClock                clock;
static mqtt::Client<MyConfig> client{transport, clock};

void setup()
{
    // Name the handler. It must outlive the client -- see the trap below.
    static auto on_message = [](const mqtt::Message& m) {
        handle(m.topic, m.payload);
    };
    client.on_message(on_message);

    mqtt::ConnectOptions opts;
    opts.client_id    = etl::string_view("sensor-07");
    opts.keep_alive_s = 30;
    client.connect(opts);          // returns immediately, does no I/O
}

// 4. Call this often -- several times per keep-alive interval at minimum.
void loop()
{
    const mqtt::Error rc = client.step();
    if (rc != mqtt::Error::Ok)
        schedule_reconnect();      // the session ended; rc says why

    if (client.is_connected() && reading_ready())
        client.publish(etl::string_view("sensors/07/temp"),
                       etl::string_view(reading),
                       mqtt::QoS::AtLeastOnce);
}
```

`step()` drives transport connect, transmit, receive, dispatch, keep-alive,
retransmission and re-subscription. It never blocks and never calls back into
itself.

## Two traps, both consequences of not allocating

**Received data is a view, not a copy.** `Message::topic` and `Message::payload`
point into the receive buffer. The moment your handler returns, those bytes may
be reused for the next packet.

```cpp
static etl::string<64> last_topic;

static auto on_message = [](const mqtt::Message& m) {
    last_topic.assign(m.topic.data(), m.topic.size());   // copy what you keep
    // storing m.topic itself would dangle
};
```

**Callbacks are non-owning.** `etl::delegate` stores a pointer to the callable
rather than a copy of it, precisely so that setting a handler cannot allocate.
A temporary dangles immediately:

```cpp
client.on_message([](const mqtt::Message& m) { ... });   // WRONG: temporary

static auto handler = [](const mqtt::Message& m) { ... };
client.on_message(handler);                              // right
```

## Reading the return values

`step()` returns `Error::Ok` for "fine, including nothing to do". Anything else
means the session just ended, and the same value already went to your
`on_disconnect` handler.

Back-pressure is **not** an error. If the transport will not accept bytes,
`step()` returns `Ok` and the data stays queued. Check `tx_pending()` if you
want to throttle.

Capacity errors are the normal way a statically-sized client says "not right
now", so handle them rather than asserting:

| Error | Means | Reasonable response |
|---|---|---|
| `NoInflightSlot` | QoS > 0 window full | retry on a later `step()` |
| `TxQueueFull` | transmit buffer full | retry, or raise `tx_buffer_size` |
| `PayloadTooLarge` | exceeds `max_persisted_msg_size` | send at QoS 0, or raise the limit |
| `NoSubscriptionSlot` | subscription table full | raise `max_subscriptions`, or use a wildcard |

## Reconnecting

There is no built-in reconnect policy: back-off is specific to your power budget
and data plan.

```cpp
if (client.state() == mqtt::State::Idle && backoff_expired())
    client.connect(opts);
```

Subscriptions are remembered across a reconnect and re-sent automatically when
the broker reports it did not keep the session.

## Where to go next

- [Configuration](Configuration) — every capacity knob, with sizing guidance
- [Porting](Porting) — implementing `Transport` and `Clock` for your stack
- [Architecture](Architecture) — the state machine and buffer strategy
- [API reference](API-Reference) — generated from the headers
