# Architecture

## The shape of the thing

Five layers, each usable without the one above it.

```
                 your application
                        |
   +--------------------+--------------------+
   |            Client<Config>               |  client.hpp
   |   state machine, QoS flows, keep-alive  |
   +--------+--------------------+-----------+
            |                    |
   +--------+--------+  +--------+--------+
   |     codec       |  |    TxQueue      |    codec.hpp, tx_queue.hpp
   | encode / decode |  |  outbound FIFO  |
   +--------+--------+  +-----------------+
            |
   +--------+--------+  +-----------------+
   |     packet      |  |     topic       |    packet.hpp, topic.hpp
   | types, headers, |  | wildcard match  |
   |      VBI        |  |                 |
   +-----------------+  +-----------------+
            |
   +-----------------------------------------+
   |        Transport, Clock (yours)         |  transport.hpp
   +-----------------------------------------+
```

The codec is a pure function library: bytes in, value types out, no state and no
I/O. You can use it on its own to build a bridge, a test tool or a protocol
analyser without instantiating a client at all. `Client` is the only stateful
component, and its state is entirely inside the object.

## Ownership and lifetimes

The client owns every buffer it uses. It never takes ownership of anything you
give it, and it never hands you anything it expects you to free.

| Data | Owner | Valid for |
|---|---|---|
| `ConnectOptions` and everything it points at | you | the `connect()` call only |
| Publish topic and payload | you | the `publish()` call only |
| Subscription filter | copied into the client | until unsubscribed |
| `Message::topic`, `Message::payload` | the client's receive buffer | the callback only |
| Callbacks (`etl::delegate`) | you | until replaced or the client dies |

The two that bite people:

**Received messages are views.** `Message::topic` and `Message::payload` point
into the receive buffer. The moment your handler returns, the client is free to
reuse those bytes for the next packet. Copy what you intend to keep. This is a
deliberate choice — the alternative is either a copy on every message or an
allocation, and neither is acceptable at this size.

**Callbacks are non-owning.** `etl::delegate` stores a pointer to the callable,
not a copy of it, precisely so that setting a handler cannot allocate. A lambda
passed as a temporary dangles immediately:

```cpp
client.on_message([](const mqtt::Message& m) { ... });   // WRONG: temporary

static auto handler = [](const mqtt::Message& m) { ... };
client.on_message(handler);                              // right
```

## The state machine

```
                       connect()
        Idle ──────────────────────────► Connecting
         ▲                                    │
         │                                    │ Transport::connect() == Ok
         │                                    │ (CONNECT already queued)
         │                                    ▼
         │                              AwaitingConnack
         │                                    │
         │ shutdown(reason)                   │ CONNACK accepted
         │ (transport error,                  ▼
         │  keep-alive timeout,           Connected
         │  protocol error,                   │
         │  CONNACK refusal,                  │ disconnect()
         │  timeout, or a clean               ▼
         │  disconnect completing)       Disconnecting
         │                                    │
         └────────────────────────────────────┘
                                    transmit queue drained
```

Two details worth knowing.

The CONNECT packet is serialized during `connect()`, before the transport is up,
and parked in the transmit queue. That is why `ConnectOptions` does not need to
outlive the call, and it means the handshake costs no extra buffering.

`Disconnecting` exists so a graceful `disconnect()` actually puts the DISCONNECT
packet on the wire before closing. Skipping that state is what causes brokers to
fire the will message on a clean shutdown. `abort()` skips it deliberately, for
when you *want* the will published.

## What `step()` does, in order

```
1. if Connecting        -> drive Transport::connect(), check the connect deadline
2. flush transmit queue -> Transport::send() until it blocks or empties
3. if Disconnecting and queue empty -> shut down, done
4. receive              -> up to 8 recv() rounds, parse and dispatch each
                           complete packet, generating acks as we go
5. if AwaitingConnack   -> check the connect deadline, done
6. if Connected         -> keep-alive, re-subscription, retransmission
7. flush transmit queue -> again
```

Step 7 is not redundant. Steps 4 and 6 are the ones that generate PUBACK,
PUBREC, PUBCOMP, PINGREQ and retransmissions; without a second flush every one
of those would wait for the next `step()`, adding a full scheduling period of
latency to the acknowledgements the protocol's delivery guarantees depend on.

The 8-round receive cap in step 4 bounds how long `step()` can run. A broker
delivering a large retained-message backlog cannot monopolise your superloop;
it just takes several iterations to drain.

`step()` returns `Error::Ok` for "everything is fine, including nothing to do".
Any other value means the session just ended, and the same value has already
been passed to your `on_disconnect` handler. Back-pressure — a transport that
will not accept bytes — is *not* an error; the data stays queued and `step()`
returns `Ok`.

## Why `step()` always returns

The memory guarantees get the attention, but on a device with a watchdog the
question that decides whether this library is usable is a different one: if I
call `step()` between two other jobs, do I get control back, and when.

Four things make the answer yes, and all four are structural rather than
careful:

**The receive path has a round budget.** `pump_rx()` will call
`Transport::recv()` at most `kMaxRecvRoundsPerStep` times before returning,
however much the peer has queued. A broker that always has another packet ready
gets eight buffers' worth of attention per step and no more; the rest waits for
the next call. This is the only place in the client where the peer could
otherwise set the pace.

**The drain loop cannot spin.** `drain_rx()` is written `for (;;)`, which looks
like the opposite of a guarantee, but every path through the body either
returns or consumes at least one whole packet and shifts the remainder down.
The buffer strictly shrinks, so the loop is bounded by how many packets fit in
`rx_buffer_size` — a compile-time number.

**There is no recursion.** Nothing on the protocol path calls itself, directly
or through the packet dispatcher: `handle_packet()` fans out to handlers that
return. `topic_matches()` is iterative for the same reason, and says so. Stack
depth is therefore a property of the call graph, not of the input.

**There is nothing to block on.** No allocation, so no allocator to wait for.
No locks, so no contention and no priority inversion. No syscalls, because the
transport is yours and is required never to block. `step()` can be slow only in
proportion to work it actually did.

Taken together the cost of a step is a function of the configuration you chose,
not of the peer's behaviour — which is the same claim `sizeof(Client<Cfg>)`
makes about memory, applied to time.

### What is measured, and what is not

The cross-compile job builds with `-fstack-usage` and reads GCC's per-function
frame sizes back out. It **fails** if any frame is dynamically sized, which
would mean a VLA or `alloca` and an unbounded stack demand. Since there is none,
and since nothing recurses, the sum of every frame is a hard upper bound on
what the library can ask of the stack. It is a loose bound — no real call path
reaches every function at once — but it is a bound, and it is published per
target rather than asserted.

Two honest limits. It does not include whatever your `Transport` needs beneath
it, which on a TLS stack will dwarf this. And a *worst-case execution time* in
cycles is a different exercise needing a call-graph tool and a target model;
what is claimed here is termination and a bound on work, not a number of
microseconds.

`tests/test_step_bounds.cpp` holds the behavioural half: a broker that never
stops talking, and the assertion that the work done per `step()` does not grow
with how much it has queued.


## Buffers

**Receive** is a single linear buffer. Bytes accumulate at the tail; complete
packets are parsed in place and the remainder is memmoved down. Parsing in place
is what lets `Message` be a view rather than a copy. A packet larger than the
buffer is a hard error (`Error::PacketTooLarge`) rather than something the
client silently truncates — see `docs/configuration.md` for sizing.

**Transmit** is a linear FIFO with compaction, not a ring buffer. The transport
wants one contiguous span per `send()`, and a ring would either hand it two
spans or force packets to straddle the wrap point. When the queue drains
completely the indices reset to zero, so in steady state compaction never runs
at all.

**Inflight** is a fixed array of slots. Each slot holds the serialized packet
awaiting acknowledgement, so retransmission needs nothing from the application:

```
publish(QoS 1)   slot.packet = [PUBLISH]  ──PUBACK──►  slot freed
publish(QoS 2)   slot.packet = [PUBLISH]  ──PUBREC──►  slot.packet = [PUBREL]
                                          ──PUBCOMP─►  slot freed
```

Reusing the slot's buffer for PUBREL rather than allocating a second one is why
the footprint stays flat across the QoS 2 handshake, and why the peak cost is
`max_inflight_out × max_persisted_msg_size` and not more.

## Where the allocations went

Paho MQTT C, receiving one QoS 1 PUBLISH: allocate the packet struct, allocate
the topic copy, allocate the payload copy, allocate a list node to queue it,
free them all after the callback. Publishing one: allocate `Publications`,
`Messages`, the topic and payload copies, a list node, and the serialization
buffer.

Here, the same two operations touch:

| Operation | Storage used |
|---|---|
| Receive a PUBLISH | the receive buffer it already arrived in |
| Deliver it | two pointers and two lengths on the stack |
| Send a PUBACK | four bytes of the transmit queue |
| Publish QoS 0 | the transmit queue |
| Publish QoS 1/2 | one preallocated inflight slot, plus the transmit queue |

Nothing is allocated because nothing new is needed — the memory was reserved
when you chose the config, and the only question at run time is whether there is
a free slot. When there is not, you get `Error::NoInflightSlot` immediately
rather than an allocation failure three layers down.

## Error handling without exceptions

Every fallible operation returns `mqtt::Error`, and operations that produce a
value return `Result<T>`:

```cpp
template <typename T> class Result
{
    constexpr bool  ok() const noexcept;
    constexpr Error error() const noexcept;
    constexpr T     value() const noexcept;        // T{} if !ok()
    constexpr T     value_or(T fallback) const noexcept;
};
```

`value()` on a failed `Result` returns a default-constructed `T` rather than
terminating. A mis-checked call degrades to a zero rather than killing the
device, which on a field-deployed node is the less bad failure.

ETL is a good fit for the same reason: its byte streams report overrun as
`false` or an empty `etl::optional`, so the whole serialization path is
exception-free without any configuration. Build ETL without
`ETL_THROW_EXCEPTIONS` and its assertion path stays out of the way too.

## Protocol strictness

The codec rejects, as protocol errors, several things Paho tolerates:

| Input | Why it is rejected |
|---|---|
| Non-minimal VBI (`0x80 0x00`) | ambiguous framing; a peer can desynchronise the stream |
| Wrong mandated flags on PUBREL/SUBSCRIBE/UNSUBSCRIBE | strong signal we are misaligned in the stream |
| QoS 3 in a PUBLISH header | reserved by the spec |
| DUP set on a QoS 0 PUBLISH | forbidden by §3.3.1.1 |
| Reserved bits set in CONNACK flags | forbidden by §3.2.2.1 |
| Packet id 0 where one is required | forbidden by §2.3.1 |
| Server sending CONNECT/SUBSCRIBE/PINGREQ/DISCONNECT | client-to-server only |
| Malformed UTF-8 in any string field | forbidden by §1.5.3; see below |

On a device that has to stay up unattended, failing loudly on a desynchronised
stream and reconnecting beats guessing at what the peer meant and delivering
corrupted messages to the application.

### UTF-8

MQTT strings are UTF-8 (§1.5.3), and `is_valid_mqtt_string()` in `utf8.cpp`
enforces it on both sides: a malformed inbound string is `MalformedPacket` and
ends the session, as the spec requires of a receiver, and the encoder refuses
to emit one for the same reason it refuses `DUP` at QoS 0 — those entry points
are documented as API and must not build a packet the peer is obliged to
reject.

What that rules out, beyond the obvious truncated and stray bytes: the
five- and six-byte forms RFC 3629 removed, code points above U+10FFFF, the
UTF-16 surrogate halves, U+0000 (§1.5.3-2 — well-formed UTF-8 and still
forbidden), and **overlong encodings**.

The overlongs are the ones that matter. An overlong lets one value be spelled
in more bytes than it needs, so `sport/+` and an overlong rendering of the same
characters are different byte strings naming the same topic. A broker whose
access control matches on bytes and a client that decodes to characters then
disagree about which topic is under discussion, and the disagreement is
attacker-chosen. This is the same reasoning that makes `vbi_decode` reject
non-minimal variable byte integers, applied one layer up.

Two things are deliberately allowed. **U+FEFF** — §1.5.3-3 says a receiver must
not skip or strip a byte order mark, so it has to accept one. And **control
characters**, which the spec only says a sender *should not* include; rejecting
on a SHOULD would drop traffic a conforming peer is entitled to send.

There is no option to switch this off. It costs a few hundred bytes of flash —
`utf8.cpp` has its own row in the
[Memory footprint](Memory-Footprint) table, so the number is measured per
target rather than argued about — and a configuration switch whose wrong
setting produces a silently non-conformant client is worth more than that in
documentation, test matrix and support.
