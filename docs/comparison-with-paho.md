# Comparison with Eclipse Paho MQTT C

This project exists because Paho MQTT C is built for a different machine than
the one many embedded projects ship on. None of what follows is a criticism of
Paho — it is a good library, and the choices it makes are the right ones for
hosted systems with a heap, threads and an OS. They are simply not available on
a Cortex-M0 with 32 KB of RAM and a watchdog.

## At a glance

| | Paho MQTT C | paho-cpp-static |
|---|---|---|
| Language | C99 | C++17 |
| Allocation | `malloc`/`free` throughout | none after construction |
| Containers | `LinkedList`, `Tree`, custom tracking heap | fixed arrays, `etl::vector` |
| Concurrency | spawns threads, mutexes, condvars | single-threaded `step()` |
| Network | BSD sockets + OpenSSL compiled in | `Transport` interface you implement |
| Exceptions | n/a (C) | none; `-fno-exceptions` clean |
| MQTT versions | 3.1, 3.1.1, 5.0 | 3.1.1 |
| Transports | TCP, TLS, WebSocket, HTTP/SOCKS proxy | whatever you implement |
| Persistence | in-memory or on-disk, pluggable | none |
| API surface | `MQTTClient` (sync) + `MQTTAsync` | one `Client` |
| Source size | ~35 kLOC | ~2.4 kLOC library, ~6.3 kLOC with tests |
| Error reporting | `int` codes + global heap state | `Error` enum, `Result<T>` |

## Allocation, concretely

The difference is easiest to see by counting.

**Paho, receiving one QoS 1 PUBLISH.** `MQTTPacket_Factory` allocates the
`Publish` struct; `readUTFlen` allocates the topic; the payload is allocated and
copied; a `List` node is allocated to queue the message; the application
callback runs; all of it is freed. Roughly five allocation/free pairs per
message, plus whatever `MQTTPersistence` does if it is enabled.

**Paho, publishing one QoS 1 message.** `MQTTProtocol_startPublish` allocates a
`Publications` struct plus copies of topic and payload,
`MQTTProtocol_createMessage` allocates a `Messages`, a `List` node goes on the
outbound queue, and `MQTTPacket_send_publish` allocates the serialization
buffer. Then the retry path may allocate again.

**Here, the same two operations.**

| Operation | Storage |
|---|---|
| Receive a PUBLISH | the receive buffer it already arrived in |
| Deliver it to the app | two pointers and two lengths, on the stack |
| Send the PUBACK | 4 bytes of the transmit FIFO |
| Publish QoS 0 | the transmit FIFO |
| Publish QoS 1/2 | one preallocated inflight slot + the transmit FIFO |

Zero allocations, because nothing new is ever needed. The memory was committed
when you picked the config. The question at run time is only whether a slot is
free, and if it is not you get `Error::NoInflightSlot` immediately and can
decide what to do — rather than an allocation failure surfacing three layers
down at the least convenient moment.

This is enforced, not asserted: `tests/test_no_alloc.cpp` replaces global
`operator new` with a counter and drives a full session through it.

## Threading

Paho's async client runs a background thread per client plus a shared socket
thread, coordinated with mutexes and condition variables, and invokes your
callbacks from those threads. On a hosted system that is convenient. On a small
target it means an RTOS, two more stacks to size, priority inversion to think
about, and callbacks arriving on a thread that may not be allowed to touch your
application state.

Here there are no threads. `step()` does everything and returns, callbacks run
on your thread inside `step()`, and you can reason about the whole client with a
single-threaded mental model. If you want it on its own RTOS task, put it on one
— but that is your decision, not the library's.

The cost is that you must call `step()` regularly. In practice this is what
embedded code does anyway.

## Bitfields on the wire

Paho models the MQTT fixed header as a union of a byte and a bitfield struct,
with the field order swapped by `#if defined(REVERSED)` on big-endian Linux:

```c
typedef union {
    char byte;
    struct {
        bit retain : 1;
        unsigned int qos : 2;
        bit dup : 1;
        unsigned int type : 4;
    } bits;
} Header;
```

Bitfield layout within a storage unit is implementation-defined in C and C++.
This works on the compilers Paho targets, but it is a portability hazard that
has to be rediscovered on each new toolchain.

`FixedHeader` here uses explicit shifts and masks, so the same code produces the
same bytes on every compiler and target, and `from_byte()` additionally
validates the flag bits against the packet type — which catches stream
desynchronisation early instead of letting a misaligned parse continue.

## What was dropped, and why

**MQTT 5.0.** The property system is the single largest chunk of state in a v5
client, and it is fundamentally variable-length — in Paho, `MQTTProperties` is a
`realloc`-grown array. Supporting it statically means capping the property count
and each property's size, which is doable but roughly doubles the client's
complexity and footprint. Most embedded deployments use 3.1.1. If you need v5,
the codec is structured so properties can be added as a fixed-capacity array
without disturbing the rest.

**WebSocket.** Framing, masking, HTTP upgrade, base64 and SHA-1 — Paho spends
about 45 kB of source on it. It exists to get MQTT through corporate proxies,
which is rarely the constraint on a device with a cellular modem. If you need
it, it belongs in a `Transport` wrapper, not in the client.

**Persistence.** Paho can spill inflight messages to disk so they survive a
process restart. Most microcontrollers have no filesystem, and those that do
want to control flash wear themselves. The inflight window here is RAM-backed
and survives reconnects but not resets. If you need reset-durable QoS, wrap
`publish()` in your own journal against whatever storage you actually have.

**HTTP/SOCKS proxy support.** Same reasoning as WebSocket: a transport concern.

**The sync/async split.** Paho ships two complete client implementations because
the blocking API cannot be built on the non-blocking one without threads. With a
`step()` core there is one implementation, and blocking behaviour — if you want
it — is a five-line loop in your code.

**Automatic reconnection.** Paho's `MQTTAsync` has a built-in reconnect with
exponential back-off. Back-off policy is genuinely application-specific
(battery, duty cycle, cellular data cost), and reimplementing it is three lines:

```cpp
if (client.state() == mqtt::State::Idle && backoff_expired())
    client.connect(opts);
```

Subscriptions are remembered and re-sent automatically, so a reconnect restores
your session without any bookkeeping on your side.

## What was made stricter

Several inputs Paho accepts are rejected here as protocol errors: non-minimal
variable byte integer encodings, wrong mandated flag bits on
PUBREL/SUBSCRIBE/UNSUBSCRIBE, QoS 3, DUP on a QoS 0 PUBLISH, reserved bits set
in CONNACK, and server-to-client packets that only make sense in the other
direction.

The reasoning: on a device that has to stay up unattended, a desynchronised
stream is far more likely than a broker that means something clever by a
non-canonical encoding. Failing fast and reconnecting produces a clean session;
guessing produces corrupted messages delivered to the application, which is much
harder to diagnose from the field.

## When to use Paho instead

Use Paho MQTT C if you need MQTT 5.0 today, if you need WebSocket or proxy
support, if you want on-disk persistence, if you are on a hosted system where a
heap and threads are free, or if you want a library with a decade of production
mileage and an Eclipse Foundation support structure behind it.

Use this one if allocation after startup is unacceptable, if you need to
certify or audit a bounded-memory system, if you are porting to a stack Paho
does not support, or if you want C++ without wrapping a C API.
