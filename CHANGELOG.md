# Changelog

Notable changes to this project, newest first.

The version line is **0.x**, where a breaking change is what a minor bump is
for. There is no 1.0 promise yet, so removing or renumbering something public
costs a minor version rather than a major one.

`CMakeLists.txt` is the single source of truth for the version: `release.yml`
refuses a tag that disagrees with it. The entries here are a record, not a
second declaration that could drift.

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
