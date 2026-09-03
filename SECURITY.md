# Security policy

## Reporting a vulnerability

**Please do not open a public issue.**

Report privately, either way:

- GitHub → the **Security** tab → *Report a vulnerability* (private advisory).
- Email **julian.wingert@subtilitas.de** with `paho-cpp-static` in the subject.

Include what you have: affected version or commit, a description, and a
reproducer if there is one. A malformed packet that reaches `drain_rx()` is the
most valuable kind of report this project can receive.

## What to expect

This is a small project with one maintainer, so the honest commitment is a
modest one rather than an impressive one:

| | |
|---|---|
| Acknowledgement | within 7 days |
| Initial assessment | within 14 days |
| Fix or a stated plan | within 90 days of the assessment |
| Disclosure | coordinated — a GitHub advisory published with the fix, crediting you unless you ask otherwise |

If a deadline is going to slip you will be told, rather than left waiting. If
you do not hear back within 14 days, assume the mail did not arrive and open a
public issue saying only that you are trying to make private contact.

## Supported versions

The most recent release, and `main`. Fixes are not backported to older tags.

Upgrading within a major version does not require source changes — see
[docs/compatibility.md](docs/compatibility.md) — so the supported version is
the latest one on your major line.

## Scope

**In scope.** Anything in `include/mqtt/` and `src/`: memory safety in the
decoders, a packet from a hostile or broken broker that causes out-of-bounds
access, an infinite or unbounded loop in `step()`, a protocol confusion that
lets a peer desynchronise the state machine, or any path that allocates.

**In scope, with a caveat.** `examples/`. These are worked references rather
than shipped code — `tls_transport.hpp` in particular is an adapter skeleton —
but a security bug in an example gets copied into real products, so report it
and it will be treated as real.

**Out of scope.** Anything in `tests/` or `tools/`, which run only in CI.
Vulnerabilities in the [Embedded Template Library](https://github.com/ETLCPP/etl)
belong upstream; report them there, and tell us too so the pinned version can be
moved.

## For integrators

Two things worth knowing before you ship this in a product.

The maintainer is an individual, not an organisation, and this library is not
made available in the course of a commercial activity. Under the EU Cyber
Resilience Act that means there is no "manufacturer" and no "open-source
steward" upstream of you: if you place a product containing this code on the
market, the Article 13 obligations — due diligence on the component, vulnerability
remediation, and a support period of at least five years — are yours, not
ours. Plan on being able to patch this code yourself. It is MIT-licensed
precisely so you can.

The practical corollary: pin a commit, not a branch, and read the diff when you
move. Notification of a fix goes out through GitHub advisories, so watch the
repository if you depend on it.
