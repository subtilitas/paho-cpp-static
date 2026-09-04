# paho-cpp-static

A minimal MQTT 3.1.1 client in C++17 for targets that cannot afford a heap at
run time.

[![CI](https://github.com/subtilitas/paho-cpp-static/actions/workflows/ci.yml/badge.svg)](https://github.com/subtilitas/paho-cpp-static/actions/workflows/ci.yml)
[![CodeQL](https://github.com/subtilitas/paho-cpp-static/actions/workflows/codeql.yml/badge.svg)](https://github.com/subtilitas/paho-cpp-static/actions/workflows/codeql.yml)
[![SCA](https://github.com/subtilitas/paho-cpp-static/actions/workflows/sca.yml/badge.svg)](https://github.com/subtilitas/paho-cpp-static/actions/workflows/sca.yml)
[![codecov](https://codecov.io/gh/subtilitas/paho-cpp-static/branch/main/graph/badge.svg)](https://codecov.io/gh/subtilitas/paho-cpp-static)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

- **No allocation after construction.** Every buffer and table is a member array
  sized by a compile-time config. `sizeof(Client<Cfg>)` is the entire cost.
- **No exceptions, no RTTI.** Clean under `-fno-exceptions -fno-rtti` on GCC and
  Clang, and with `/EHsc` and `/GR` removed plus `_HAS_EXCEPTIONS=0` on MSVC. A
  `#error` in `error.hpp` checks the compiler's own `__cpp_exceptions` /
  `_CPPUNWIND`, so a flag that failed to apply is a build failure. Every failure
  path returns an `mqtt::Error`.
- **No OS dependency.** No threads, mutexes, sockets or `<chrono>`. You
  implement two interfaces; the client never blocks.
- **`step()` returns in bounded time.** The receive path caps its `recv()` calls
  per step, the drain loop strictly shrinks its buffer each pass, and nothing on
  the protocol path recurses. CI measures stack usage on the target and fails on
  a dynamically sized frame.
- **C++ throughout** — no C wrapper, no `void*` context pointers.
- **Serialization on the [Embedded Template Library](https://www.etlcpp.com/)**,
  via `etl::byte_stream_writer` / `etl::byte_stream_reader`, which report
  overruns as `bool` / `etl::optional` rather than throwing.

About 2 200 lines of library and 1 KiB of RAM at the small end. Flash, RAM and
stack are measured on every push, on the host and cross-compiled for Cortex-M0+
and Cortex-M4 — see
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint)
and the [Cross-compile](https://github.com/subtilitas/paho-cpp-static/actions/workflows/cross.yml)
job summaries.

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

## Installing it

`add_subdirectory()` and `FetchContent` both work — link `mqtt::client`, which
carries the ETL include path with it. To install instead:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build
cmake --install build
```

```cmake
find_package(paho-cpp-static 1.0 REQUIRED)
target_link_libraries(app PRIVATE mqtt::client)
```

ETL appears in this library's public headers, so an installed package has to
say where ETL comes from rather than leave a consumer to find it. How it was
satisfied is decided when the library is built and recorded in the generated
config: a build that fetched ETL installs those exact headers alongside the
library, in a directory of their own, and points at them; a build given
`-DMQTT_ETL_DIR=...` records that the consumer manages ETL and requires
`find_package(etl)`, saying so plainly if it is missing. ETL installs nothing of
its own when it is not the top-level project, which is why the first is the
default.

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

Two consequences of not allocating:

- **`Message::topic` and `Message::payload` are views into the receive buffer**,
  valid only for the duration of the callback. Copy what you keep.
- **Callbacks are non-owning.** A callback slot stores a pointer to the
  callable, not a copy, so the callable must outlive the client. Passing a
  temporary is a compile error rather than a dangling pointer — name it
  (`static`, or a member) and pass that.
- **A handler may end the session.** `abort()` and `disconnect()` are safe from
  inside one, and the rest of that `step()` is abandoned. `subscribe()` and
  `unsubscribe()` are not — they mutate the table being walked. `step()` itself
  returns `Error::Reentrant` rather than recursing.

## Porting

The platform layer is two interfaces:

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
| `winsock_transport.hpp` | the same, on Winsock. Link `ws2_32` |
| `tls_transport.hpp` | the same interface around mbedTLS |

### Platforms

The library has no OS dependency, so a platform is a `Transport` and a `Clock`
and nothing else.

| Platform | Library and tests | Examples |
|---|---|---|
| Linux, GCC and Clang | built and tested on every push | all three demos |
| macOS, Clang | built and tested on every push | all three demos |
| Windows, MSVC | built and tested on every push | `ip_publisher`, over `winsock_transport.hpp` |
| Cortex-M0+ and Cortex-M4, `arm-none-eabi` | built, with flash, RAM and stack measured on every push | none — no sockets to build against |

`ip_publisher` selects the Winsock pair on Windows and the POSIX pair
elsewhere, and is otherwise the same program. The other two demos use
`getaddrinfo` through `<netdb.h>` and are built on Unix only.

See [docs/porting.md](docs/porting.md) for lwIP, Zephyr, FreeRTOS+TCP and
AT-command modem notes.

## Memory

| Profile | `sizeof(Client<Cfg>)` |
|---|---|
| Sensor — QoS 0 only, 256 B buffers | 1032 B |
| `DefaultConfig` | 4544 B |
| Gateway — 4 KiB buffers, 16 inflight, 32 subs | 23 368 B |

Flash is 7.7 KiB (7866 bytes) for the non-template core, plus 8.9 KiB (9160
bytes) for a `Client<Cfg>` instantiation touching every public entry point —
less in a real application, since the linker drops what you never call.

These are x86-64 GCC at `-Os`, indicative rather than a promise for Cortex-M.
Exact figures are remeasured on every push and published to
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint),
which takes precedence if the two ever disagree.

One deliberate trade-off: each inflight QoS > 0 slot owns a
`max_persisted_msg_size` buffer holding the serialized packet, so retransmission
needs nothing from the application. It costs
`max_inflight_out × max_persisted_msg_size` bytes, and it is why `publish()`
returns `Error::PayloadTooLarge` for a QoS > 0 message that will not fit. QoS 0
bypasses it entirely.

[docs/configuration.md](docs/configuration.md) documents every knob with sizing
guidance and worked profiles.

## Scope

**Implemented.** CONNECT/CONNACK with will and credentials, PUBLISH at QoS 0/1/2
in both directions with full acknowledgement handshakes and DUP retransmission,
SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP keep-alive, DISCONNECT,
topic wildcard matching, UTF-8 validation of every string field in both
directions (§1.5.3, overlong encodings and surrogates included), and automatic
re-subscription after a session the broker did not retain.

**Absent by choice.** MQTT 5.0 properties, WebSocket transport, HTTP/SOCKS
proxying, on-disk persistence, MQTT 3.1 (protocol level 3), and automatic
reconnection policy. TLS is a `Transport` implementation rather than a
compiled-in dependency.

Reconnection is left to the application because back-off policy is specific to
its power budget and data cost:

```cpp
if (client.state() == mqtt::State::Idle && backoff_expired())
    client.connect(opts);
```

Subscriptions are remembered and re-sent automatically, regardless of
`clean_session`: that flag governs what the *broker* keeps, while the client's
table records what the application asked for. Call `unsubscribe()` to forget a
filter.

[docs/comparison-with-paho.md](docs/comparison-with-paho.md) gives the
differences from Eclipse Paho MQTT C, with allocation counts for the same
operations in both libraries.

## Testing

A dependency-free harness rather than GoogleTest, which reports failures by
throwing and would undercut the `-fno-exceptions` guarantee. Every case name and
the live count are on the generated
[Test inventory](https://github.com/subtilitas/paho-cpp-static/wiki/Test-Inventory)
page, rebuilt from the suite on every push.

| File | Covers |
|---|---|
| `tests/test_packet.cpp` | variable byte integers, fixed header flag validation |
| `tests/test_codec.cpp` | encode/decode round trips, golden bytes, malformed input |
| `tests/test_encode_bounds.cpp` | every encoder against every output buffer size |
| `tests/test_topic.cpp` | wildcard matching against the spec's own examples, and against a second implementation over every short filter/topic pair |
| `tests/test_utf8.cpp` | overlongs, surrogates, truncation, U+0000, the BOM |
| `tests/test_tx_queue.cpp` | FIFO reserve, commit, consume, compaction |
| `tests/test_client.cpp` | handshakes, QoS flows, keep-alive, fragmentation, teardown |
| `tests/test_step_bounds.cpp` | work per `step()` does not grow with what the peer queues |
| `tests/test_config_profiles.cpp` | the client instantiates and runs at each documented profile |
| `tests/test_to_string.cpp` | every enumerator has a distinct name and no fallthrough |
| `tests/test_error_values.cpp` | the `Error` numbering, pinned at compile time and at run time |
| `tests/test_callback_reentrancy.cpp` | what a handler may do to the client that is calling it |
| `tests/test_no_alloc.cpp` | global `operator new` replaced with a counter |

Three more cases are builds rather than runs, under `tests/compile_fail/`: a
callback slot must reject a temporary callable and accept a named one, and a
guard like that produces a build error rather than a wrong answer. CTest runs
each by building it, two expecting failure and one expecting success — the
third is the control, without which a slot that rejected everything would pass.

`tests/consumer/` is built out of tree against the *installed* package, because
`add_subdirectory()` and `FetchContent` both bypass the generated config file
and would leave a broken one invisible.

`test_no_alloc.cpp` is the load-bearing one. It replaces global `operator new`
with a counting version, arms it, and drives a full session — connect,
subscribe, publish at all three QoS levels, inbound traffic, keep-alive,
retransmission, table exhaustion, a malformed-packet teardown, disconnect — then
asserts the counter is zero. A separate case proves construction does not
allocate, so a `Client` can live in `.bss` on a target with no heap linked.

### Coverage

<!-- coverage:start -->
| Metric | Covered | Total | Measured |
|---|---|---|---|
| Lines | 1224 | 1295 | 94.5% |
| Branches | 852 | 961 | 88.7% |
| Functions | 623 | 823 | 75.7% |
<!-- coverage:end -->

Measured by `gcovr` on every push and published to
[Codecov](https://codecov.io/gh/subtilitas/paho-cpp-static). The table is
written by `tools/coverage.py`; CI re-measures and runs `--check`, failing the
build when these figures drift from what the suite does. Scope is
`include/mqtt/` and `src/`; tests, examples and the fetched ETL checkout are
excluded, since counting them would flatter the number.

Function coverage is reported but not gated: the suite instantiates the client
at several configurations, and each instantiation creates its own set of
template functions, so testing more configurations lowers that percentage while
raising every other one.

**The Codecov badge reads lower than `gcovr`, and both are right.** gcovr counts
a line as covered if it executed at all. Codecov also tracks *partials* — a line
whose branches were only partly taken, such as an `if` that was never false —
and a partial is not a hit. The badge is therefore closer to branch coverage;
move it by testing the untaken side of a condition, not by executing more lines.

Instrumentation is `PUBLIC` because most of this library is templates in
headers, so the code under test compiles into the *test* objects rather than
into `libpaho_cpp_static.a`. Measuring only the library target would report a
healthy figure for four small `.cpp` files and say nothing about `client.hpp`.

CI fails below a floor in `.github/workflows/ci.yml` (`COVERAGE_MIN_LINE`,
`COVERAGE_MIN_BRANCH`), set a couple of points under where the suite sits so
ordinary churn does not trip it. The gate is `gcovr`'s own `--fail-under-*` on
the runner, not a Codecov status, so the build does not depend on a third-party
service.

Reproduce it locally:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMQTT_COVERAGE=ON -DMQTT_BUILD_EXAMPLES=OFF
cmake --build build --parallel
ctest --test-dir build
gcovr --root . --filter include/mqtt/ --filter src/ --print-summary \
      --json-summary coverage.json
python3 tools/coverage.py --summary coverage.json --check
```

`--check` is what CI runs; `--write` updates the table after a change that
legitimately moves it.

Also verified:

- Clean under `-fsanitize=address,undefined`.
- Clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wcast-align`.
- clang-tidy and clang-format gate on every push; CodeQL runs on every push and
  reports to the Security tab. See
  [docs/static-analysis.md](docs/static-analysis.md).
- `nm` on the library and on a full `Client` instantiation shows no reference to
  `malloc`, `operator new`, `__cxa_throw`, `_Unwind_*` or any typeinfo symbol.
- Interoperates with two independent brokers at QoS 0/1/2 in both directions,
  on every push. **Mosquitto**, with the broker's own trace confirming complete
  PUBLISH/PUBREC/PUBREL/PUBCOMP handshakes and keep-alive pings and no protocol
  errors logged. **[minimosq](https://github.com/subtilitas/minimosq-mqtt)**,
  which reports protocol violations, refusals, failed sends and dropped
  deliveries through an observer rather than a log, so that job asserts on typed
  events instead of matching words. One implementation's leniency is not a
  specification; two disagreeing is a finding.

## Documentation

The [wiki](https://github.com/subtilitas/paho-cpp-static/wiki) renders
everything below, rebuilt by CI on every push. Three of its pages are generated
from the source rather than written by hand, so they cannot drift:
[API reference](https://github.com/subtilitas/paho-cpp-static/wiki/API-Reference)
from the header comments,
[Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint)
from compiling and measuring, and
[Test inventory](https://github.com/subtilitas/paho-cpp-static/wiki/Test-Inventory)
from the suite itself.

| Document | Contents |
|---|---|
| [docs/getting-started.md](docs/getting-started.md) | build, run against a broker, write your first program |
| [docs/architecture.md](docs/architecture.md) | layering, state machine, buffers, ownership rules |
| [docs/porting.md](docs/porting.md) | implementing `Transport` and `Clock`, platform notes, TLS, verifying a port |
| [docs/configuration.md](docs/configuration.md) | every config knob, sizing guidance, worked profiles |
| [docs/comparison-with-paho.md](docs/comparison-with-paho.md) | differences from Eclipse Paho MQTT C and the reasoning |
| [docs/testing.md](docs/testing.md) | what is verified and how — suite, sanitizers, coverage, fuzzing, interop, and what is not tested |
| [docs/static-analysis.md](docs/static-analysis.md) | what CodeQL, clang-tidy and clang-format report, and which of them gate |
| [docs/misra.md](docs/misra.md) | MISRA C++:2023 self-assessment — constructs measured, two deviations, what a real claim would need |
| [docs/compatibility.md](docs/compatibility.md) | what a version number promises: the covered surface, what is excluded, the `Error` numbering rule |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |

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

---

In collaboration with Claude Code.
