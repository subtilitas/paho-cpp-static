# paho-cpp-static

A minimal MQTT 3.1.1 client in C++17 for targets that cannot afford a heap at
run time.

[![CI](https://github.com/subtilitas/paho-cpp-static/actions/workflows/ci.yml/badge.svg)](https://github.com/subtilitas/paho-cpp-static/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/subtilitas/paho-cpp-static/branch/main/graph/badge.svg)](https://codecov.io/gh/subtilitas/paho-cpp-static)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

- **No allocation after construction.** Every buffer and table is a member array
  sized by a compile-time config. `sizeof(Client<Cfg>)` is the entire cost.
- **No exceptions, no RTTI.** Builds and links clean under `-fno-exceptions
  -fno-rtti` on GCC and Clang, and with `/EHsc` and `/GR` removed plus
  `_HAS_EXCEPTIONS=0` on MSVC. A `#error` in `error.hpp` checks the compiler's
  own `__cpp_exceptions` / `_CPPUNWIND`, so a flag that fails to take effect is
  a build failure rather than a quietly broken promise. Every failure path
  returns an `mqtt::Error`.
- **No OS dependency.** No threads, no mutexes, no sockets, no `<chrono>`. You
  implement two small interfaces; the client never blocks.
- **C++ throughout**, so there is no C wrapper to write and no `void*` context
  pointers to keep alive.
- **Serialization on the [Embedded Template Library](https://www.etlcpp.com/)**,
  using `etl::byte_stream_writer` / `etl::byte_stream_reader`, which report
  overruns as `bool` / `etl::optional` rather than throwing.

Roughly 2 400 lines of library, and about 1 KB of RAM at the small end. Flash
and RAM figures are measured on every push — see
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint).

---

## Quick start

```bash
git clone https://github.com/subtilitas/paho-cpp-static.git
cd paho-cpp-static
cmake -S . -B build            # fetches ETL; or pass -DMQTT_ETL_DIR=/path/to/etl
cmake --build build
ctest --test-dir build --output-on-failure
```

Then, against a local broker:

```bash
./build/examples/pubsub_demo 127.0.0.1 1883 demo/topic
```

## Using it

```cpp
#include "mqtt/client.hpp"

struct MyConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size    = 2048;
    static constexpr size_t max_inflight_out  = 8;
    static constexpr size_t max_subscriptions = 16;
};

static MyTransport            transport;   // yours -- see docs/porting.md
static MyClock                clock;       // yours
static mqtt::Client<MyConfig> client{transport, clock};

void app_init()
{
    static auto on_message = [](const mqtt::Message& m) {
        // topic and payload view the receive buffer -- copy what you keep
        handle(m.topic, m.payload);
    };
    client.on_message(on_message);

    mqtt::ConnectOptions opts;
    opts.client_id    = etl::string_view("sensor-07");
    opts.keep_alive_s = 30;
    client.connect(opts);          // returns immediately
}

void app_loop()          // superloop or RTOS task
{
    if (client.step() != mqtt::Error::Ok)
        schedule_reconnect();

    if (client.is_connected() && sample_ready())
        client.publish(etl::string_view("sensors/07/temp"),
                       etl::string_view(reading), mqtt::QoS::AtLeastOnce);
}
```

Two things that catch people out, both consequences of not allocating:

- **`Message::topic` and `Message::payload` are views into the receive buffer.**
  They are valid for the duration of the callback and no longer. Copy anything
  you intend to keep.
- **Callbacks are non-owning.** `etl::delegate` stores a pointer to the
  callable, so a lambda passed as a temporary dangles. Name it (`static`, or a
  member) rather than passing it inline.

## Porting

The entire platform layer is two interfaces:

```cpp
class Transport
{
    virtual Error connect() noexcept = 0;
    virtual Error send(etl::span<const uint8_t> data, size_t& written) noexcept = 0;
    virtual Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept = 0;
    virtual void  close() noexcept = 0;
    virtual bool  is_connected() const noexcept = 0;
};

class Clock
{
    virtual uint32_t now_ms() const noexcept = 0;   // monotonic; may wrap
};
```

Never block; return `Error::WouldBlock` when you made no progress; partial
transfers are expected and handled. TLS needs no support from the MQTT layer —
do the handshake inside `connect()`, returning `WouldBlock` until it completes.

Three worked transports ship in `examples/`:

| Header | What it does |
|---|---|
| `posix_transport.hpp` | BSD sockets with `getaddrinfo`, so it takes a hostname |
| `tcp_ip_transport.hpp` | fixed IPv4 address, no DNS and no resolver — closest to a shipped device |
| `tls_transport.hpp` | the same interface around mbedTLS |

See [docs/porting.md](docs/porting.md) for lwIP, Zephyr, FreeRTOS+TCP and
AT-command modem notes.

## Memory

| Profile | `sizeof(Client<Cfg>)` |
|---|---|
| Sensor — QoS 0 only, 256 B buffers | ~1.0 KB |
| `DefaultConfig` | ~4.6 KB |
| Gateway — 4 KB buffers, 16 inflight, 32 subs | ~23 KB |

Flash is roughly 6.9 KB for the non-template core, plus about 9 KB for a
`Client<Cfg>` instantiation that touches every public entry point — considerably
less in a real application, since the linker drops what you never call.

These are x86-64 GCC at `-Os` and indicative rather than a promise for
Cortex-M. Exact figures are **remeasured on every push** and published to
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint),
which is the number to trust if this table and that page ever disagree.

The one deliberate trade-off: each inflight QoS > 0 slot owns a
`max_persisted_msg_size` buffer holding the serialized packet, so retransmission
needs nothing from your application. That costs
`max_inflight_out × max_persisted_msg_size` bytes, and it is why `publish()`
returns `Error::PayloadTooLarge` for a QoS > 0 message that will not fit. QoS 0
bypasses it entirely.

[docs/configuration.md](docs/configuration.md) documents every knob with sizing
guidance and worked examples for sensor, reliable-sensor and gateway profiles.

## Scope

Implemented: CONNECT/CONNACK with will and credentials, PUBLISH at QoS 0/1/2 in
both directions with full acknowledgement handshakes and DUP retransmission,
SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP keep-alive, DISCONNECT,
topic wildcard matching, and automatic re-subscription after a session the
broker did not retain.

Deliberately absent: MQTT 5.0 properties, WebSocket transport, HTTP/SOCKS
proxying, on-disk persistence, MQTT 3.1 (protocol level 3), and automatic
reconnection policy. TLS is supported as a `Transport` implementation rather
than a compiled-in dependency.

Reconnection is left to you on purpose — back-off policy is application-specific
(battery, duty cycle, data cost), and it is three lines:

```cpp
if (client.state() == mqtt::State::Idle && backoff_expired())
    client.connect(opts);
```

Subscriptions are remembered and re-sent automatically, so a reconnect restores
your session without bookkeeping on your side. This holds regardless of
`clean_session`: that flag governs what the *broker* keeps, while the client's
table is its own record of what you asked for. Call `unsubscribe()` to forget a
filter.

[docs/comparison-with-paho.md](docs/comparison-with-paho.md) explains what was
dropped and why, with allocation counts for the same operations in both
libraries.

## Testing

A dependency-free harness — deliberately not GoogleTest, since a framework that
reports failures by throwing would undercut the `-fno-exceptions` guarantee. The
live count and the name of every case are on the generated
[Test inventory](https://github.com/subtilitas/paho-cpp-static/wiki/Test-Inventory)
page, which is rebuilt from the suite itself on every push.

| File | Covers |
|---|---|
| `tests/test_packet.cpp` | variable byte integers, fixed header flag validation |
| `tests/test_codec.cpp` | encode/decode round trips, golden bytes, malformed input |
| `tests/test_topic.cpp` | wildcard matching against the spec's own examples |
| `tests/test_client.cpp` | handshakes, QoS flows, keep-alive, fragmentation, teardown |
| `tests/test_to_string.cpp` | every enumerator has a distinct name and no fallthrough |
| `tests/test_no_alloc.cpp` | global `operator new` replaced with a counter |

`test_no_alloc.cpp` is the one that matters. It replaces global `operator new`
with a counting version, arms it, and drives a full session — connect,
subscribe, publish at all three QoS levels, inbound traffic, keep-alive,
retransmission, table exhaustion, a malformed-packet teardown, disconnect — then
asserts the counter is still zero. A separate case proves construction does not
allocate either, so a `Client` can live in `.bss` on a target with no heap
linked at all.

### Coverage

Measured by `gcovr` on every push and published to
[Codecov](https://codecov.io/gh/subtilitas/paho-cpp-static) — the badge above is
the live figure, which is why no number is written out here. Scope is
`include/mqtt/` and `src/` only; tests, examples and the fetched ETL checkout are
excluded, since counting them would flatter the number rather than measure it.

Instrumentation is applied `PUBLIC`, which matters more than it sounds: most of
this library is templates in headers, so the code under test is compiled into
the *test* objects rather than into `libpaho_cpp_static.a`. Measuring only the
library target would report a healthy figure for four small `.cpp` files and say
nothing whatever about `client.hpp`, where the state machine lives.

CI fails below a floor set in `.github/workflows/ci.yml` (`COVERAGE_MIN_LINE`,
`COVERAGE_MIN_BRANCH`), deliberately a couple of points under where the suite
sits so ordinary churn does not trip it and raising it is a decision rather than
a drift. The gate is `gcovr`'s own `--fail-under-*` on the runner, not a Codecov
status, so the build does not depend on a third-party service being reachable.

Reproduce it locally:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMQTT_COVERAGE=ON -DMQTT_BUILD_EXAMPLES=OFF
cmake --build build --parallel
ctest --test-dir build
gcovr --root . --filter include/mqtt/ --filter src/ --print-summary
```

Also verified:

- Clean under `-fsanitize=address,undefined`.
- Clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wcast-align`.
- `nm` on the library and on a full `Client` instantiation shows no reference to
  `malloc`, `operator new`, `__cxa_throw`, `_Unwind_*` or any typeinfo symbol.
- Interoperates with Mosquitto at QoS 0/1/2 in both directions, with the
  broker's own trace confirming complete PUBLISH/PUBREC/PUBREL/PUBCOMP
  handshakes and keep-alive pings, and no protocol errors logged. CI runs this
  on every push.

## Documentation

The [wiki](https://github.com/subtilitas/paho-cpp-static/wiki) is the rendered
version of everything below, rebuilt by CI on every push. It also carries three
pages that are *generated from the source* rather than written by hand:
[API reference](https://github.com/subtilitas/paho-cpp-static/wiki/API-Reference)
from the header comments,
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint)
from compiling and measuring, and
[Test inventory](https://github.com/subtilitas/paho-cpp-static/wiki/Test-Inventory)
from the suite itself — so none of them can quietly drift out of date.

| Document | Contents |
|---|---|
| [docs/getting-started.md](docs/getting-started.md) | build, run against a broker, write your first program |
| [docs/architecture.md](docs/architecture.md) | layering, state machine, buffers, ownership rules, where the allocations went |
| [docs/porting.md](docs/porting.md) | implementing `Transport` and `Clock`, platform notes, TLS, how to verify a port |
| [docs/configuration.md](docs/configuration.md) | every config knob, sizing guidance, worked profiles |
| [docs/comparison-with-paho.md](docs/comparison-with-paho.md) | differences from Eclipse Paho MQTT C and the reasoning |

## Layout

```
include/mqtt/
  error.hpp        Error enum, Result<T>, no-exception plumbing
  config.hpp       compile-time capacities and their static_asserts
  packet.hpp       packet types, fixed header, variable byte integer
  codec.hpp        serialization API
  transport.hpp    Transport and Clock interfaces (the porting layer)
  topic.hpp        wildcard matching
  tx_queue.hpp     fixed-capacity outbound byte FIFO
  client.hpp       the state machine
src/               non-template implementations
tests/             harness, fakes, broker simulator, suites
examples/          three transports (hostname, fixed IPv4, mbedTLS) and demos
docs/              architecture, porting, configuration, comparison
```

## Licence

MIT — see [LICENSE](LICENSE).

The name references Eclipse Paho because this project answers the question
"what would Paho MQTT C look like if it could not use a heap?", but **no Paho
source code is present here**. The client is an independent implementation
written against the OASIS MQTT 3.1.1 specification. See
[NOTICE.md](NOTICE.md) for the full provenance statement and third-party
notices.
