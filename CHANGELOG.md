# Changelog

Notable changes to this project, newest first.

The version line is **0.x**, where a breaking change is what a minor bump is
for — the same rule `docs/static-analysis.md` states for the API. There is no
1.0 promise yet, so removing or renumbering something public costs a minor
version rather than a major one.

`CMakeLists.txt` is the single source of truth for the version. `release.yml`
refuses a tag that disagrees with it, and `tools/pins.py` prints what is
pinned, so the number here is a record rather than a second declaration that
could drift.

## [0.6.0] — unreleased

### Removed

- **`Error::NoInboundSlot`.** It was declared and named by `to_string()` but
  never returned by anything. The QoS 2 inbound path it was written for
  deliberately stays silent when the tracking table is full — acknowledging
  would let the broker drop a message that was never delivered — and reports
  the condition through `inbound_overflow_count()` instead. The enumerator was
  the fossil of a decision the code had already argued its way out of.

  **Breaking.** This renumbers every `Error` enumerator after it:
  `NoSubscriptionSlot` through `TransportClosed` each shift down by one, and
  `TransportClosed` is now 21. Code that compares against the names is
  unaffected; anything that persisted or transmitted the numeric value needs
  rebuilding against this header.

### Added

- Measured coverage figures in the README, refreshed by `tools/coverage.py`
  and checked on every push. The tool has a `--check` mode that fails on
  drift rather than a bot that rewrites the README, because a file that edits
  itself is one nobody reads the diff of.

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
