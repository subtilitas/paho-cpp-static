# Changelog

Notable changes to this project, newest first.

From 1.0.0 the project follows semantic versioning: a major bump is the only
thing that may break code using the covered surface correctly.
[docs/compatibility.md](docs/compatibility.md) states what is covered, what is
not, and the rules the `Error` numbering follows.

Releases before 1.0.0 were a 0.x line, where a breaking change cost a minor
version rather than a major one. None of those guarantees applied to them.

`CMakeLists.txt` is the single source of truth for the version: `release.yml`
refuses a tag that disagrees with it. The entries here are a record, not a
second declaration that could drift.

## [1.0.0-rc4] — 2026-09-04

A configuration value that did not mean what it said. Found by the same
external suite, in a round that found nothing in the code rc3 added.

### Fixed

- **`max_pending_acks = 0` and `max_inflight_in = 0` behaved as if set to one.**
  Every table is rounded up by `detail::at_least_one` so that a zero-sized array
  is never declared, and these two capacity guards tested the storage rather
  than the configured limit. So `= 0` neither refused the operation nor
  reclaimed anything: `subscribe()` returned `Ok` and consumed the rounded-up
  slot, and an inbound QoS 2 message was tracked with
  `inbound_overflow_count()` left at zero.

  `max_inflight_out` and `max_subscriptions` already read the configured value.
  All four now agree: zero means none, and the operation is refused rather than
  quietly given one slot.

  The storage does not entirely disappear — a zero-length array is ill-formed,
  so each table still declares one element that nothing is put into. `config.hpp`
  now says so, and says what zero does for each capacity that accepts it.

  Predates 1.0.0-rc1. Low severity: no memory error and no overflow, and the
  reading that `0` should mean "none" was an inference from the one documented
  case rather than a stated contract.

- **`config.hpp` overstated what `max_inflight_out = 0` reclaims.** It said
  "all of the persisted-message storage", but `at_least_one` rounds the
  outbound slot array up as well, and that one remaining slot carries a
  `max_persisted_msg_size` buffer. Zeroing `max_inflight_out` alone takes
  `sizeof(Client<DefaultConfig>)` from 4552 to 3752 bytes and leaves a 256-byte
  buffer nothing can reach; zeroing `max_persisted_msg_size` with it reaches
  3496. The worked sensor profile in `docs/configuration.md` already sets both;
  the header now says why that matters.

## [1.0.0-rc3] — 2026-09-03

Three defects in the code rc2 added, found by re-running the same adversarial
suite against the fixes themselves. Both rc1 findings stay closed.

### Fixed

- **A handler that ends a session and starts another wrote the new CONNECT to
  the closed transport.** `abort()` then `connect()` from a handler is how an
  application reacts to a reconfigure command, and rc2 made ending a session
  from a handler supported rather than merely tolerated. But
  `transport_.connect()` runs only at the top of `step()`, while handlers run
  inside `pump_rx()` — so the rest of that pass operated on a transport that
  `close()` had torn down. Against a transport that refuses writes after
  `close()`, as a socket does, the new session died before its CONNECT left the
  queue: `on_disconnect` fired twice for one application action, and `step()`
  reported `TransportFailure`.

  `pump_rx()` and `step_once()` both stop when a handler has left the client in
  `Connecting`. The next `step()` starts at the top and establishes the
  transport.

  rc2's own fix opened this path: `drain_rx` returns `last_error_` when it finds
  the buffer moved under it, and `connect()` resets `last_error_` to `Ok`, so
  the pass continued instead of stopping.
- **`dispatch()` kept delivering after a handler ended the session.** Two
  filters can match one message, and the handlers after the one that called
  `abort()` were told about a message that arrived on a connection which no
  longer exists — every client call they made on the strength of it refused
  with `NotConnected`. The fan-out now stops. The documented promise gains its
  exception: delivery to each matching handler, in table order, until one ends
  the session.
- **`step()`'s contract did not admit `Error::Reentrant`.** It documented that
  any value other than `Ok` means the session has ended and the same value went
  to `on_disconnect`; neither is true of the code rc2 appended. An outer loop
  written the way the docstring invited — `if (step() != Ok) reconnect();` —
  would tear down a healthy connection the first time a handler called `step()`.
  Documented as the exception it is. `is_retryable()` deliberately does not
  cover it: retrying is what a handler must not do.

### Changed

- The test binary is built with `-Werror` under `MQTT_WERROR`, as the library
  already is. `-Wswitch` had reported a test whose handler silently did nothing
  because an enumerator was missing from a switch, and a warning is easy to
  scroll past in a wall of build output.

## [1.0.0-rc2] — 2026-09-03

Three memory-safety defects and one silent delivery failure, all found in
1.0.0-rc1 by an adversarial suite kept outside this repository. No public
declaration changes except one appended `Error` code, which the compatibility
promise allows.

### Fixed

- **`drain_rx` handed `memmove` a negative length when a handler ended the
  session.** The packet size is measured before the handler runs and subtracted
  from `rx_len_` after. `abort()` reaches `shutdown()`, which sets `rx_len_` to
  zero, so the `size_t` subtraction wrapped to near `SIZE_MAX`. One QoS 0
  PUBLISH and a handler that calls `abort()` is enough, on `DefaultConfig`.
  Reachable from documented application code: the handler rules named
  `subscribe()` and `unsubscribe()` as the things not to do, and `abort()` is
  documented as the way to drop a connection so the will fires.

  `drain_rx` now detects that the buffer moved under it and stops, rather than
  clamping and draining on into a session that has ended.
- **`handle_suback` erased through a pointer the callback had invalidated.**
  The SUBACK handler ran the callback and then retired the pending-ack entry;
  a callback that ends the session reaches `shutdown()`, which clears
  `pending_`, leaving the entry pointer naming an element of an empty vector.
  The entry is now retired before the callback.
- **`step()` called from a handler recursed without bound.** It re-drained the
  same bytes, re-entered the same handler and ran the stack out — contradicting
  the bounded-stack guarantee this project measures on target in CI. A nested
  call now returns `Error::Reentrant` and does nothing.
- **A topic filter ending in `/` matched nothing, including its own topic.**
  `topic_matches` advanced past the trailing separator and left the loop before
  comparing the empty final level on each side, so a subscription to `sensors/`
  received nothing. MQTT 3.1.1 §4.7.1 permits a zero-length level, both
  validators accept these strings, and a broker forwards them — so the failure
  was silent: no error code, no log line, no memory error. 476 filter/topic
  pairs were affected, every one a false negative.

### Added

- **`Error::Reentrant` (22)**, appended. Under the compatibility promise a new
  code is appended and no existing number moves; `TransportClosed` is still 21.
- **`tests/test_callback_reentrancy.cpp`** — seven cases covering what a
  handler may do to the client that is calling it. Every one fails under
  AddressSanitizer or UndefinedBehaviorSanitizer against rc1.
- **A differential check for `topic_matches`**, comparing it with a second
  implementation over every filter/topic pair up to length four that both
  validators accept. This is what a wrong-but-clean answer needs: no sanitizer,
  coverage gate or oracle-less fuzzer can see a function that returns the wrong
  result without misbehaving.
- **Fuzz handlers that act on the client.** `fuzz_client`'s handlers did
  nothing, which left every defect above structurally unreachable — 100,000
  runs found none of them. They now publish, disconnect, abort, re-enter
  `step()` and read the accessors, chosen from the input; against rc1 the
  target reproduces the `memmove` defect in under 30,000 runs. The checked-in
  corpus grew from 1187 to 1452 covered edges.

### Changed

- The handler documentation states what a handler may do, rather than only what
  it may not.

## [1.0.0-rc1] — 2026-09-03

First release candidate for 1.0. The API is frozen as described in
[docs/compatibility.md](docs/compatibility.md); this candidate exists so that
freeze can be found wrong before it becomes a promise.

### Added

- **A compatibility policy.** What a version number covers, what it does not,
  and the rule the `Error` numbering now follows.
- **Explicit `Error` values**, pinned by `tests/test_error_values.cpp` at
  compile time and again at run time. Removing an enumerator renumbers every
  one after it, which is invisible at every call site and silently wrong for
  anything that persisted or transmitted the value. It happened once, in 0.6.0.
  It now fails the build.
- **A temporary callable is rejected at compile time.** A callback slot stores
  a pointer to the callable, so one bound to a temporary was left pointing at
  nothing. ETL 20.48.1 deletes construction from an rvalue callable, but its
  constraint exempts anything convertible to a function pointer — which a
  capture-less lambda is, and a capture-less lambda is what a caller writes
  inline. `mqtt::detail::Handler` closes that case. `sizeof(Client<Cfg>)` is
  unchanged. Three cases under `tests/compile_fail/` hold it, including a
  positive control so a slot that rejected everything could not pass.
- **`install(EXPORT)` and `find_package(paho-cpp-static)`.** ETL appears in the
  public headers, so the generated config states where ETL comes from rather
  than guessing: a build that fetched ETL installs those exact headers beside
  the library and points at them; a build given `MQTT_ETL_DIR` requires
  `find_package(etl)` and says so if it is missing. `tests/consumer/` is built
  out of tree against the install prefix on every push, with the build tree
  deleted first so a leaked build path fails there rather than downstream.
- **Fuzzing.** libFuzzer targets over the decoders and over the whole receive
  path, with a minimised corpus in `tests/fuzz/corpus/`. CI replays the corpus
  as a deterministic gate and then explores for three minutes. 200 000 codec
  and 100 000 client executions under ASan and UBSan found nothing.
- **A second broker in interop.** The client runs against
  [minimosq](https://github.com/subtilitas/minimosq-mqtt) as well as Mosquitto.
  minimosq reports protocol violations, refusals, failed sends and dropped
  deliveries through an observer, so that job asserts on typed events rather
  than on words in a log.
- **A Winsock transport**, `examples/winsock_transport.hpp`, with
  `examples/win_clock.hpp`. `ip_publisher` selects the Windows pair or the
  POSIX pair and is otherwise the same program, so the Windows CI job compiles
  the transport on every push. The IPv4 parser moved to `examples/ipv4.hpp`,
  shared by both transports rather than duplicated.
- **Prerelease versioning.** `MQTT_VERSION_PRERELEASE` in `CMakeLists.txt`
  carries the suffix that `project(VERSION ...)` cannot hold. `release.yml`
  checks a tag against the joined version and marks the release a prerelease
  from the declaration, so a tag alone cannot turn a candidate into a final
  release.

### Changed

- **ETL 20.39.4 → 20.48.1.** This moves every measured RAM figure: sensor
  1064 → 1032, `DefaultConfig` 4600 → 4544, reliable sensor 3624 → 3568,
  gateway 23 456 → 23 368 bytes. Flash is unchanged at 7866 bytes for the
  non-template core. Each subscription still costs `max_topic_len + 64` bytes.
- Memory quantities are written in KiB and MiB. `4 KB buffers` meant 4096
  bytes.

### Fixed

- **The README understated flash.** It said "roughly 6.9 KB for the
  non-template core"; it measures 7866 bytes, 7.7 KiB, and did so before the
  ETL bump as well. Anyone sizing a part from that figure was 900 bytes short.
- `docs/static-analysis.md` described the `Error` enum's size in terms of a
  change made in 0.2.0, quoting figures that no longer held.

## [0.6.1] — 2026-09-01

### Fixed

- **Corrected memory figures in `docs/configuration.md`.** The worked profiles
  quoted sizes that no longer matched the code, and the per-subscription cost
  was understated. `sizeof(Client<Cfg>)` is now stated in bytes and measured:
  sensor 1064 (was "well under 1 KB"), reliable 3624, gateway 23 456 (was
  "roughly 21 KB", and in disagreement with the README's own table). Each
  subscription costs `max_topic_len + 64` bytes, not `+ 24`. Anyone who sized a
  build from the old gateway or subscription numbers was under-budgeting.
- Corrected other measured claims across the documentation: the count of
  disabled clang-tidy checks (21, not fifteen), the sample footprint in
  `docs/getting-started.md` (2552 bytes, stale since the `Error` enum shrank),
  the library and test line counts, and the README's claim that clang-tidy and
  clang-format are advisory when both have gated since 0.2.0.
- The README's test table listed 6 of the 11 suites; it now lists all of them.

### Changed

- Documentation rewritten for concision. No technical content was removed.

## [0.6.0] — 2026-08-31

### Removed

- **`Error::NoInboundSlot`.** Declared and named by `to_string()`, but never
  returned by anything. The QoS 2 inbound path it was written for deliberately
  stays silent when the tracking table is full — acknowledging would let the
  broker drop a message that was never delivered — and reports the condition
  through `inbound_overflow_count()` instead.

  **Breaking.** Renumbers every `Error` enumerator after it:
  `NoSubscriptionSlot` through `TransportClosed` each shift down by one, and
  `TransportClosed` is now 21. Code comparing against the names is unaffected;
  anything that persisted or transmitted the numeric value must be rebuilt
  against this header.

### Added

- Measured coverage figures in the README, written by `tools/coverage.py` and
  checked on every push. `--check` fails on drift rather than a bot rewriting
  the README.

## [0.5.0] — 2026-08-31

### Added

- UTF-8 validation of every string field in both directions (MQTT-1.5.3),
  including overlong encodings, surrogate halves and U+0000.
- A stated, tested and measured guarantee that `step()` always returns: a
  receive round budget, a strictly shrinking drain loop, no recursion, and a
  CI check that fails on a dynamically sized stack frame.
- CycloneDX and SPDX bills of materials, generated from the pin rather than
  stored alongside it.
- A MISRA C++:2023 self-assessment, with the constructs measured rather than
  asserted.
- A weekly job that watches whether the pinned ETL tag has been moved.

### Changed

- ETL is pinned by commit rather than by tag, and `tools/pins.py --check`
  enforces that no workflow cache key restates it differently.

## [0.3.0] — 2026-08-24

### Added

- Cross-compilation to Cortex-M0+ and Cortex-M4, with flash, RAM and stack
  measured on every push.
- A bare-metal `arm-none-eabi` toolchain file.
- A security policy.
- Considerably wider tests: the transmit queue, the codec's limits, the
  untaken side of the validation guards, and a sweep of the encoder buffer
  sizes and the configuration space.

### Fixed

- The footprint tool derives `nm` from the longest matching compiler suffix,
  so it finds the cross toolchain's binary rather than the host's.
- The Mosquitto interop job is bounded and can no longer hang.

## Before 0.3.0

The version declared in `CMakeLists.txt` was briefly incoherent. On
2026-08-19 it was bumped to 1.3.0 and then 1.5.0, which was a mistake — this
has always been a 0.x library — and it was corrected back to 0.2.0 the same
day. Neither 1.3.0 nor 1.5.0 was ever tagged or released, so the only
artefacts that exist are the ones listed above. Those commit subjects still
read `chore: release 1.5.0` and `chore: release 0.2.0` in the log, because
rewriting published history to tidy a message costs more than the confusion
it saves. This section is the correction.

`v0.4.0` was skipped rather than withdrawn: 0.3.0 went straight to 0.5.0.

The first release under the current CMake build was 0.1.0, on 2026-08-13.
