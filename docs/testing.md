# Testing

What is verified, how, and what is not. Every number here is produced by a
command in this file or by a job in `.github/workflows/`, and CI re-derives the
ones that can drift.

## The in-tree suite

174 cases, 1246 checks, in a header-only harness (`tests/test_harness.hpp`) with
no external framework. The library is compiled `-fno-exceptions`, so a framework
that reports failures by throwing would undermine the property under test.

The per-suite breakdown and the full list of case names are generated from the
sources on every push — see the
[Test inventory](https://github.com/subtilitas/paho-cpp-static/wiki/Test-Inventory)
page rather than a copy here, which would drift.

Run it:

```bash
cmake -S . -B build -DMQTT_WERROR=ON -DMQTT_BUILD_EXAMPLES=ON
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

`MQTT_WERROR=ON` builds the library and the tests with `-Werror`. It is off by
default so a consumer's build is not broken by a new compiler's new
diagnostics, and on in every CI job.

### Zero allocation

`test_no_alloc.cpp` replaces global `operator new` and fails if it is called
during construction, a full session, or the error and exhaustion paths. This is
the reason the suite is excluded from the sanitizer job: replacing the allocator
conflicts with ASan intercepting it.

### Bounded work per step

`test_step_bounds.cpp` asserts that one `step()` does a bounded amount of work
regardless of how much the peer has queued, that a flooded client still makes
progress across steps, and that a receive buffer packed with whole packets
drains in one step. A transport emitting 8 KiB is drained across at most 64
steps. The budget defers work; it does not drop it.

### Wraparound

Two counters wrap, and both are reachable by a device that is only left
running. `test_wraparound.cpp` drives each to its boundary rather than asserting
that the arithmetic looks right.

- **The packet id** is 16 bits and must skip 0, so it wraps from 65535 to 1
  after 65535 QoS > 0 publishes — about 18 hours at one per second. The cases
  walk the whole space, check that 0 is never issued, that an id still in flight
  is not reissued as the allocator wraps past it, and that a QoS 2 handshake
  taking 65535 completes across the boundary.
- **The millisecond clock** is 32 bits and wraps every 49.7 days. Five fields
  read it, and `elapsed_ms()` relies on unsigned subtraction being well defined
  rather than on the clock being monotonic in the large. Every timer is started
  at `2^32 - 5000` so it crosses the boundary mid-scenario: keep-alive pings
  once, still times out when no PINGRESP arrives, retransmission fires once
  rather than once per elapsed interval, the connect timeout measures correctly,
  and `ms_since_last_receive()` stays small.

### Compile-failure tests

`mqtt::detail::Handler` refuses a temporary callable, because the delegate it
wraps stores a pointer rather than owning it. That guard produces a build error
rather than a wrong answer, so it cannot be checked from a normal test. Three
targets in `tests/compile_fail/` are built by CTest: two must fail to compile
and one must succeed. The positive control is what stops a `Handler` that
rejected every callable from passing.

## Sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer, `halt_on_error=1`, on every
push:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DMQTT_WERROR=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san --parallel && ctest --test-dir build-san -E no_alloc
```

## Platforms

| Target | Built | Tested |
|---|---|---|
| Linux / GCC | yes | full suite |
| Linux / Clang | yes | full suite |
| macOS / Clang | yes | full suite |
| Windows / MSVC | yes | full suite |
| Cortex-M0+ (ARMv6-M), `arm-none-eabi` | yes | flash, RAM and stack measured; not run — no sockets |
| Cortex-M4 (ARMv7E-M), `arm-none-eabi` | yes | flash, RAM and stack measured; not run — no sockets |

The cross job measures worst-case stack per function with `-fstack-usage` and
fails on growth, which is what backs the claim that no protocol path recurses.

## Coverage

Measured by gcovr 7.2 on every push, against floors of 92% line and 85% branch:

| | Covered | Total | % |
|---|---|---|---|
| Lines | 1221 | 1293 | 94.4% |
| Branches | 850 | 961 | 88.4% |
| Functions | 585 | 766 | 76.4% |

Function coverage is reported and not gated. The client is a template
instantiated at several configurations, so each instantiation counts separately
and a config exercised by one case reports the rest of its functions as
uncovered. `tools/coverage.py --check` fails the build if the figures in
`README.md` drift from the measurement by more than 0.5 points.

The gcovr version changes the answer, so it is pinned. On identical coverage
data, gcovr 7.2 reports 94.4% of 1293 lines and gcovr 8.6 reports 44.7% of
8906: the two disagree about how to count a line in an instantiated template,
and 8.6 counts each instantiation separately. A local measurement with the
wrong version looks exactly like a large regression.

## Fuzzing

Two libFuzzer targets, clang only:

- `fuzz_codec` — the decoder against arbitrary bytes. Corpus: 11 inputs.
- `fuzz_client` — a full session driven by fuzzer-chosen transport behaviour.
  Corpus: 166 inputs.

CI runs both twice. The **gate** replays the checked-in corpus, which is
deterministic because `-runs` is a count rather than a clock, so it either
passes or names an input. The **search** runs 180 s per target at
`-max_len=256`; it is not a gate in the same sense, since it explores a
different amount on every run and fails only when it finds something. A crash
uploads the input that reproduces it.

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DMQTT_FUZZ=ON
cmake --build build-fuzz --parallel
ctest --test-dir build-fuzz -L fuzz
```

libFuzzer writes new inputs into the corpus directory it is given. Minimise with
`-merge=1` into a clean directory before committing one.

## Interoperability

Against two brokers on a real TCP socket, on every push:

- **Mosquitto**, installed from the distribution package.
- **minimosq**, built against the published headers tarball rather than a
  source tree, so the artefact under test is the one a consumer downloads.

Both run `pubsub_demo`, which checks nine stages: CONNECT accepted, SUBSCRIBE
acknowledged, five messages published cycling QoS 0, 1 and 2, the QoS 1 and
QoS 2 handshakes completed, all five echoed back through the subscription, the
connection held open by keep-alive alone, DISCONNECT completed, the socket
closed, and the session ended with `Error::Ok`. minimosq's observer additionally
reports protocol violations, refusals, failed sends and dropped deliveries, and
the job fails on any of them.

## The installed package

A separate job installs the library and builds `tests/consumer/` out of tree
against the install prefix only. Building inside the repository proves nothing
about `install(EXPORT)`: `add_subdirectory()` and `FetchContent` both bypass the
generated config, so a broken one stays invisible until a consumer hits it.

## Static analysis

clang-tidy and clang-format gate. CodeQL is advisory — it reports two
false-positive classes on this codebase, described in
[static-analysis.md](static-analysis.md). `tools/scan_constructs.py --check`
measures the MISRA C++:2023 constructs the code claims to avoid and fails on
budget growth; [misra.md](misra.md) states the two deviations and what a real
compliance claim would need.

`tools/pins.py --check` fails if the version in `CMakeLists.txt` disagrees with
any workflow reference, and a scheduled job verifies that the pinned ETL commit
is still the one its tag names.

## External adversarial review

Five rounds of an adversarial suite held outside this repository, each written
against a specific release candidate and re-run against the fix.

- Rounds 1 to 3 found **six defects**, four of which affected a caller. Two were
  introduced by the previous round's fixes, which is the argument for re-running
  a suite against its own repairs rather than only against new code.
- Rounds 4 and 5 found none.
- The suite seeds 10 sub-suites from 15 random seeds: **454,789,024 checks, 0
  failures**. Before round 5 every seed was a hard-coded fallback, so the
  earlier per-round figure of 30,313,589 checks was one trajectory walked
  repeatedly rather than accumulating coverage.
- `topic_matches` is compared differentially against a real broker's own
  matcher over 7,011 (filter, topic) pairs — every string of up to three levels
  over `{a, b, empty}` for topics and `{a, b, empty, +}` for filters, plus
  hand-written edge cases — with **0 disagreements**. The comparison is
  mutation-checked: a build where `+` spans a separator reports 1,570
  disagreements on the same set, so a clean result is not a vacuous one.
- The numbers this documentation quotes are themselves re-measured, so a claim
  an integrator sizes against cannot drift from the code silently.

## What is not tested

Stated rather than implied.

- **TLS.** Every interop run is clear text on a loopback socket. The
  `Transport` interface is where TLS belongs and [porting.md](porting.md)
  describes it, but no test exercises a TLS transport.
- **A hostile broker.** Both interop brokers are correct implementations. The
  in-tree fake transport covers malformed packets and refused writes; a peer
  that is adversarial rather than merely broken is not modelled.
- **A second toolchain under the external suite.** It runs GCC 13.3.0 on
  x86-64 only. The four host compilers above are covered by CI, not by it.
- **Long fuzzing campaigns.** 180 s per target per push is a smoke test. No
  multi-hour campaign has been run.
- **Hardware.** The Cortex-M targets are built and measured, never executed.
  No figure here comes from a device.
- **Throughput on a real network.** All timing figures come from a simulated
  clock or a loopback socket.
