# Configuration and sizing

Everything the client uses is sized at compile time from a config type. There
are no run-time knobs, no hidden pointers and no growth: `sizeof(Client<Cfg>)`
is the whole cost, readable from a linker map.

## Declaring a config

C++17 has no class-type non-type template parameters, so the config is passed as
a *type*. Derive from `mqtt::DefaultConfig` and override what you need:

```cpp
struct MyConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size    = 2048;
    static constexpr size_t max_inflight_out  = 8;
    static constexpr size_t max_subscriptions = 16;
};

mqtt::Client<MyConfig> client{transport, clock};
```

Anything not overridden keeps its default. `ConfigCheck<Cfg>`, instantiated by
`Client`, turns nonsensical combinations into build errors.

## The knobs

### Capacities set to zero

`max_inflight_out`, `max_inflight_in`, `max_subscriptions` and
`max_pending_acks` accept `0`, and zero means none rather than the one slot the
storage still holds.

For the three that bound a call the application makes, that means the call is
refused: `Error::NotSupported` from `publish()` above QoS 0,
`Error::NoSubscriptionSlot` from `subscribe()`, `Error::NoPendingAckSlot` from
`subscribe()` and `unsubscribe()`. `max_inflight_in` bounds what the *broker*
sends, and MQTT 3.1.1 has no way to refuse an inbound QoS. At zero the client
declines to track the packet id, which means it never acknowledges the message
and the broker retransmits it — see `max_inflight_in` below.

The storage does not go away entirely. A zero-length array is ill-formed, so
each table still declares one element that nothing is ever put into. That
residue is 2 bytes for `max_inflight_in`, `max_topic_len + 64` bytes for
`max_subscriptions`, and one outbound slot including its
`max_persisted_msg_size` buffer for `max_inflight_out`. Set
`max_persisted_msg_size` to `0` alongside `max_inflight_out = 0` to shrink that
buffer to a single byte.

Measured on x86-64 with GCC at `-Os`, against `sizeof(Client<DefaultConfig>)`
of 4552 bytes and changing one thing at a time:

- `max_inflight_out = 0` — 3752 bytes, saving 800.
- `max_inflight_out = 0` and `max_persisted_msg_size = 0` — 3496 bytes, saving 1056.
- `max_subscriptions = 0` — 3656 bytes, saving 896.
- `max_pending_acks = 0` — 4432 bytes, saving 120.
- `max_inflight_in = 0` — 4552 bytes, saving nothing.

### `rx_buffer_size` — default 1024

Must hold the largest inbound packet **whole**, including its fixed header.
Packets are parsed in place, which is what makes `Message::topic` and
`Message::payload` views instead of copies.

A larger packet ends the connection with `Error::PacketTooLarge`. There is
nowhere else to put it, and truncating would corrupt the stream.

Size it as: longest topic subscribed to + largest payload received + ~8 bytes of
framing. Include **retained messages**, which arrive immediately on subscribe
and are often the largest thing a broker sends.

### `tx_buffer_size` — default 1024

The outbound byte FIFO. Must be at least as large as the biggest single packet;
beyond that it sets how much can be queued before `publish()` returns
`Error::TxQueueFull`. A larger transmit buffer is the cheapest way to ride out a
briefly stalled link. `tx_pending()` reports how full it is.

Protocol acknowledgements need room here too. With none available the client
defers the acknowledgement rather than dropping the connection: an inbound QoS 1
or QoS 2 message is left both unacknowledged **and** undelivered, so the broker
retransmits and delivery happens on the retry. Acknowledging a message that
could not be delivered would lose it; delivering one that could not be
acknowledged would duplicate it.

`tx_backpressure_count()` counts these deferrals — a tuning signal, not an
error. A persistently rising value means `tx_buffer_size` is undersized for the
traffic, or `step()` is not called often enough to drain it.

### `max_topic_len` — default 64

Longest topic name or filter in bytes, excluding any NUL, for both publishing
and subscribing. Filters are copied into the subscription table, so this is a
multiplier on its cost — see `max_subscriptions`.

### `max_client_id_len` / `max_username_len` / `max_password_len`

Validation limits for `connect()`. They cost nothing at run time, since the
CONNECT packet is serialized straight into the transmit queue and none of these
strings are retained. They buy an early, specific `Error::InvalidArgument`
instead of a rejection from the broker a round trip later.

### `max_inflight_out` — default 4

The outbound QoS > 0 window: how many QoS 1 or QoS 2 publishes may be in flight
at once. `publish()` returns `Error::NoInflightSlot` when it is full.

Set it to `0` to forbid QoS > 0 publishing: `publish()` above QoS 0 returns
`Error::NotSupported`. That alone saves 800 bytes and leaves one
`max_persisted_msg_size` buffer nothing can reach; set `max_persisted_msg_size`
to `0` alongside it to recover a further 256. See "Capacities set to zero"
above. The constrained sensor profile below sets both.

Sizing: round-trip time to the broker divided by publish interval, plus
headroom. A sensor publishing every 5 s to a broker 200 ms away needs 1. A
gateway bursting 20 messages at once needs 20, or must handle
`NoInflightSlot` by retrying.

### `max_persisted_msg_size` — default 256

Bytes reserved **per inflight slot** to hold the serialized packet for
retransmission:

```
inflight storage = max_inflight_out × max_persisted_msg_size
```

At the defaults that is 4 × 256 = 1024 bytes, about 22% of
`sizeof(Client<DefaultConfig>)` — the largest single item after the two 1 KiB
buffers.

The trade-off is deliberate: because the client owns a serialized copy, it can
retransmit with DUP set without the application keeping the payload alive and
without allocating. The cost is that a QoS > 0 publish which does not fit is
rejected with `Error::PayloadTooLarge`.

It must accommodate topic + payload + 2-byte packet id + ~5 bytes of framing.
QoS 0 publishes bypass it entirely and are bounded only by `tx_buffer_size`, so
large-but-unimportant messages still go out cheaply.

Once PUBREC arrives the slot's buffer is reused for the much smaller PUBREL, so
the footprint stays flat across the QoS 2 handshake rather than doubling.

### `max_inflight_in` — default 4

Packet ids of inbound QoS 2 messages received but not yet released by PUBREL,
for duplicate suppression, at 2 bytes each.

If it fills, an incoming QoS 2 message is neither delivered nor acknowledged,
leaving it with the broker to retransmit, and `inbound_overflow_count()`
increments. The connection is deliberately *not* dropped: doing so would hand a
peer a trivial way to knock the device offline. At a non-zero capacity, a
rising count means this is undersized for the broker's delivery rate.

Set it to `0` only on a client that never subscribes above QoS 1. Zero saves no
memory at all — the one-element residue plus struct padding absorbs the
difference, and `sizeof(Client<Cfg>)` is unchanged — and it makes inbound QoS 2
permanently undeliverable rather than transiently blocked. Every QoS 2 PUBLISH
takes the overflow branch, so the message is never delivered, the broker
retransmits it for as long as the session lasts, and its packet id occupies the
broker's inflight window. Nothing reports this. The caller gets no error because
no call was made, and MQTT 3.1.1 has no way to refuse a QoS the peer is entitled
to use. A steadily rising `inbound_overflow_count()` is the only evidence.

### `max_subscriptions` — default 8

Active subscriptions, retained across reconnects and re-sent automatically when
CONNACK reports `session_present = false`. `subscribe()` returns
`Error::NoSubscriptionSlot` when full.

Retention does not depend on `clean_session`. That flag governs the state the
*broker* keeps; the client's table is its own record of what the application
asked for, and the only thing that can rebuild the session, since the caller's
filter strings are copied in and never referenced again. `unsubscribe()` forgets
a filter, and a filter the broker refuses in SUBACK is dropped automatically.

Each entry costs `max_topic_len + 64` bytes — the copied filter plus the
delegate, ids and flags. At the defaults, 8 × 128 = 1024 bytes.

Wildcards keep this small: one subscription to `sensors/+/cmd` beats thirty
individual filters, and the client's matcher routes them to the right handler.

Set it to `0` on a publish-only client to refuse every `subscribe()` with
`Error::NoSubscriptionSlot`, saving 896 bytes at the default `max_topic_len`.

### `max_pending_acks` — default 4

SUBSCRIBE and UNSUBSCRIBE requests awaiting their ack. Rarely needs to exceed 2
unless many filters are subscribed in a burst at startup. Both calls return
`Error::NoPendingAckSlot` when full; retry on the next loop iteration.

Set it to `0` to refuse both calls outright, saving 120 bytes. A client that
only publishes needs neither, and pairing this with `max_subscriptions = 0`
saves 1016 bytes together.

### `max_topics_per_request` — default 4

Filters permitted in a single SUBSCRIBE or UNSUBSCRIBE packet, and the batch
size for automatic re-subscription after a lost session. Larger batches mean
fewer round trips at startup; smaller ones spread the transmit-queue load.

### `retry_interval_ms` — default 20000

How long before retransmitting an unacknowledged QoS > 0 packet with DUP set.
`0` disables timed retransmission, leaving messages to be re-sent only after a
reconnect.

Do not set this below the round-trip time, or messages the broker has already
acknowledged get retransmitted, wasting airtime and inflight slots. On a
cellular link, 30–60 s is more realistic than the default.

Independently of this timer, everything inflight is retransmitted immediately
after a successful CONNACK.

### `connect_timeout_ms` — default 30000

Deadline covering the whole handshake: transport connect plus CONNACK. On expiry
the session ends with `Error::ConnectTimeout`.

## Worked profiles

Figures are x86-64 GCC at `-Os`, measured with `sizeof()`.

### Constrained sensor node — telemetry only

QoS 0 publishes, one wildcard subscription for commands, small messages.

```cpp
struct SensorConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 256;
    static constexpr size_t tx_buffer_size         = 256;
    static constexpr size_t max_topic_len          = 48;
    static constexpr size_t max_client_id_len      = 20;
    static constexpr size_t max_inflight_out       = 0;    // QoS 0 only
    static constexpr size_t max_persisted_msg_size = 0;
    static constexpr size_t max_inflight_in        = 1;
    static constexpr size_t max_subscriptions      = 2;
    static constexpr size_t max_pending_acks       = 1;
    static constexpr size_t max_topics_per_request = 1;
};
```

**1032 bytes.**

### Reliable sensor node

QoS 1 telemetry that must survive a flaky link.

```cpp
struct ReliableConfig : mqtt::DefaultConfig
{
    static constexpr size_t   rx_buffer_size         = 512;
    static constexpr size_t   tx_buffer_size         = 1024;
    static constexpr size_t   max_topic_len          = 64;
    static constexpr size_t   max_inflight_out       = 8;
    static constexpr size_t   max_persisted_msg_size = 128;
    static constexpr size_t   max_subscriptions      = 4;
    static constexpr uint32_t retry_interval_ms      = 30000;
};
```

**3568 bytes**, of which 1024 is the retransmission store — the price of the
delivery guarantee, visible rather than hidden in a heap.

### Gateway

Many subscriptions, larger messages, deeper window.

```cpp
struct GatewayConfig : mqtt::DefaultConfig
{
    static constexpr size_t rx_buffer_size         = 4096;
    static constexpr size_t tx_buffer_size         = 4096;
    static constexpr size_t max_topic_len          = 128;
    static constexpr size_t max_inflight_out       = 16;
    static constexpr size_t max_persisted_msg_size = 512;
    static constexpr size_t max_inflight_in        = 16;
    static constexpr size_t max_subscriptions      = 32;
    static constexpr size_t max_pending_acks       = 8;
    static constexpr size_t max_topics_per_request = 8;
};
```

**23 368 bytes.** Fine on anything with external RAM, and still entirely static.

`tests/test_config_profiles.cpp` instantiates and exercises each of these, so
the figures above are compiled rather than remembered.

## Measuring

Assert the budget in your own build:

```cpp
static_assert(sizeof(mqtt::Client<MyConfig>) <= 4096, "MQTT client budget exceeded");
```

That is the cheapest guard against someone quietly raising a buffer size past
what the target can afford.

`tests/test_no_alloc.cpp` has a case that prints `sizeof()` for three
configurations; adding yours takes one line.
