# Compatibility

What a version number promises, and what it does not.

The library follows semantic versioning from 1.0.0 onward. A major bump is the
only thing that may break code that used the covered surface correctly.

## The covered surface

Everything below is covered. A change to any of it that breaks a correct
program requires a major version.

| Covered | Detail |
|---|---|
| Headers under `include/mqtt/` | every declaration not inside a `detail` namespace |
| `mqtt::Error` enumerator names | a name is never removed or repurposed |
| `mqtt::Error` numeric values | pinned in `tests/test_error_values.cpp`; see below |
| `mqtt::State`, `mqtt::QoS`, `mqtt::PacketType`, `mqtt::ConnackCode` | names and numeric values alike |
| `Config` member names and their meanings | a field is never removed or given a new meaning |
| `DefaultConfig` member values | changing one changes `sizeof(Client<Cfg>)` for anybody who inherits it |
| `Transport` and `Clock` | the two interfaces a port implements |
| The CMake target `mqtt::client` | its name, and that it carries the ETL include path |
| `find_package(paho-cpp-static)` | the package name and the target it defines |
| Behaviour the tests assert | the suite is the specification of the state machine |

## What is not covered

These may change in any release, including a patch.

- **Anything in a `detail` namespace**, and anything under `src/`. `detail`
  means what it says.
- **`sizeof(Client<Cfg>)` for a given `Cfg`.** It is measured on every pull
  request and merge, and republished to the wiki when the sources it derives
  from change. Measured, not promised: a field added to the client moves it.
  Size a build from the
  [Memory footprint](https://github.com/subtilitas/paho-cpp-static/wiki/Memory-Footprint)
  page, and re-read it when you upgrade.
- **Flash and stack figures.** Measured, for the same reason.
- **The ETL version.** ETL appears in this library's public headers, so its
  version is visible to a consumer, but which version is pinned is not part of
  this promise. `CMakeLists.txt` records the pin.
- **The layout of a release archive**, and the contents of the generated wiki
  pages.
- **Anything the library does with input the MQTT 3.1.1 specification forbids.**
  A malformed packet is rejected; exactly which `Error` it is rejected with may
  change.

## The `Error` numbering

`Error` is a `uint8_t` enum whose numeric values are spelled out in
`error.hpp` and pinned by `tests/test_error_values.cpp`, at compile time and
again at run time.

This matters because the value is the part that escapes the program. Code
comparing against enumerator names keeps compiling and keeps working across a
renumber; a value that was written to flash, sent over a wire, or logged as an
integer in an earlier build does not. Renumbering is invisible at every call
site and silently wrong at the one place it is not.

So, from 1.0:

- A retired code keeps its number and its slot. It is documented as retired,
  and `to_string()` keeps naming it.
- A number is never reused for a different condition.
- A new code is appended.

This happened once before the promise applied: 0.6.0 removed
`Error::NoInboundSlot` and shifted the nine codes after it down by one. The
pinned test exists so that it cannot happen again without the build failing.

## Callbacks and lifetimes

Two rules that are part of the interface rather than of any single function.

**`Message::topic` and `Message::payload` are views into the receive buffer.**
They are valid for the duration of the callback and not afterwards. Copy what
you keep. This is a consequence of not allocating and will not change.

**A callback slot stores a pointer to the callable, not a copy of it.** The
callable must outlive the client. Passing a temporary is a compile error rather
than a dangling pointer:

```cpp
client.on_message([](const mqtt::Message& m) { handle(m); });   // does not compile

static auto handler = [](const mqtt::Message& m) { handle(m); };
client.on_message(handler);                                     // fine
```

Both `on_message()` and the per-subscription handler taken by `subscribe()`
behave this way.

## Deprecation

A covered name that is going away is marked `[[deprecated]]` in one release and
removed no earlier than the next major version. The deprecation names its
replacement. Nothing is removed without having been deprecated first.

The exception is a name that was never usable — one that no code could have
called correctly. Removing it cannot break a correct program, and it is
recorded in `CHANGELOG.md` as a breaking change regardless.

## Pre-1.0 history

Before 1.0.0 the line was 0.x, where a breaking change cost a minor version
rather than a major one. `CHANGELOG.md` records what changed at each of those
releases. None of the guarantees above applied to them.
