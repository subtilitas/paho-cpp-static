# Configuration and sizing

Everything the client uses is sized at compile time from a config type. There
are no run-time knobs, no hidden pointers and no growth. `sizeof(Client<Cfg>)`
is the whole story, and you can read it off a linker map.

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

Anything you do not override keeps the default. `ConfigCheck<Cfg>` is
instantiated by `Client` and turns nonsensical combinations into build errors
rather than 3am field failures.

## The knobs

### `rx_buffer_size` — default 1024

Must hold the largest inbound packet **whole**, including its fixed header.
Packets are parsed in place, which is what makes `Message::topic` and
`Message::payload` views instead of copies.

A packet larger than this ends the connection with `Error::PacketTooLarge`. It
cannot be handled any other way — there is nowhere to put it, and silently
truncating would corrupt the stream.

Size it as: longest topic you subscribe to + largest payload you will receive +
~8 bytes of framing. Do not forget **retained messages**, which arrive
immediately on subscribe and are often the largest thing a broker sends you.

### `tx_buffer_size` — default 1024

The outbound byte FIFO. Must be at least as large as your biggest single packet;
beyond that it determines how much you can queue before `publish()` starts
returning `Error::TxQueueFull`.

A larger transmit buffer is the cheapest way to ride out a slow or briefly
stalled link. `tx_pending()` tells you how full it is, which makes a decent
back-pressure signal for your application.

Protocol acknowledgements need room here too. When there is none, the client
defers the acknowledgement rather than dropping the connection: an inbound QoS 1
or QoS 2 message is left both unacknowledged **and** undelivered, so the broker
retransmits it and delivery happens on the retry. That keeps the guarantee
intact — acknowledging a message you could not deliver would lose it, and
delivering one you could not acknowledge would duplicate it.

`tx_backpressure_count()` counts these deferrals. It is a tuning signal, not an
error: a persistently rising value means `tx_buffer_size` is undersized for your
traffic, or `step()` is not being called often enough to drain it.

### `max_topic_len` — default 64

Longest topic name or filter, in bytes, excluding any NUL. Applies both to
topics you publish to and filters you subscribe to. Costs
`max_subscriptions × max_topic_len` bytes in the subscription table, since
filters are copied there.

### `max_client_id_len` / `max_username_len` / `max_password_len`

Validation limits for `connect()`. They cost nothing at run time — the CONNECT
packet is serialized straight into the transmit queue and none of these strings
are retained — but they give you an early, specific `Error::InvalidArgument`
instead of a mysterious rejection from the broker.

### `max_inflight_out` — default 4

The outbound QoS > 0 window: how many QoS 1 or QoS 2 publishes can be in flight
at once. `publish()` returns `Error::NoInflightSlot` when it is full.

Set it to `0` to forbid QoS > 0 publishing entirely and reclaim all of the
persisted-message storage. A pure telemetry node that only publishes QoS 0 saves
real memory this way.

Sizing: round-trip time to the broker divided by your publish interval, plus
headroom. A sensor publishing every 5 s to a broker 200 ms away needs 1. A
gateway bursting 20 messages at once needs 20, or it needs to handle
`NoInflightSlot` by retrying later.

### `max_persisted_msg_size` — default 256

Bytes reserved **per inflight slot** to hold the serialized packet for
retransmission. This is the one genuinely expensive setting:

```
inflight storage = max_inflight_out × max_persisted_msg_size
```

Defaults: 4 × 256 = 1 KB, which is most of `sizeof(Client<DefaultConfig>)`.

The trade-off is deliberate. Because the client owns a serialized copy, it can
retransmit with DUP set without your application keeping the payload alive, and
without an allocation. The cost is that a QoS > 0 publish which does not fit is
rejected with `Error::PayloadTooLarge`.

It must accommodate: topic + payload + 2-byte packet id + ~5 bytes of framing.
QoS 0 publishes bypass this entirely and are bounded only by `tx_buffer_size`,
so large-but-unimportant messages can still go out cheaply.

Once PUBREC arrives the slot's buffer is reused for the much smaller PUBREL, so
the footprint stays flat across the QoS 2 handshake rather than doubling.

### `max_inflight_in` — default 4

Packet ids of inbound QoS 2 messages received but not yet released by PUBREL.
Used for duplicate suppression, at 2 bytes each — cheap.

If it fills, an incoming QoS 2 message is neither delivered nor acknowledged,
leaving it with the broker to retransmit, and `inbound_overflow_count()`
increments. The connection is deliberately *not* dropped: doing so would hand a
peer a trivial way to knock the device offline. A non-zero count means this is
undersized for your broker's delivery rate.

### `max_subscriptions` — default 8

Active subscriptions. Retained across reconnects and re-sent automatically when
CONNACK reports `session_present = false`. `subscribe()` returns
`Error::NoSubscriptionSlot` when full.

Retention does not depend on `clean_session`. That flag governs the state the
*broker* keeps; the client's own table is its record of what the application
asked for, and it is the only thing that can rebuild the session, since the
caller's filter strings are copied in and never referenced again. `unsubscribe()`
is how you forget a filter, and a filter the broker refuses in SUBACK is dropped
automatically.

Costs roughly `max_subscriptions × (max_topic_len + 24)` bytes.

Use wildcards to keep this small: one subscription to `sensors/+/cmd` beats
thirty individual filters, and the client's own matcher routes them to the right
handler.

### `max_pending_acks` — default 4

SUBSCRIBE and UNSUBSCRIBE requests awaiting their ack. Rarely needs to exceed 2
unless you subscribe to many filters in a burst at startup. `subscribe()` and
`unsubscribe()` return `Error::NoPendingAckSlot` when full — retry on the next
loop iteration.

### `max_topics_per_request` — default 4

Filters permitted in a single SUBSCRIBE or UNSUBSCRIBE packet, and the batch
size used for automatic re-subscription after a lost session. Larger batches
mean fewer round trips at startup; smaller ones spread the transmit-queue load.

### `retry_interval_ms` — default 20000

How long to wait before retransmitting an unacknowledged QoS > 0 packet with DUP
set. `0` disables timed retransmission, in which case messages are only re-sent
after a reconnect.

Do not set this below your round-trip time, or you will retransmit messages the
broker has already acknowledged and waste both airtime and inflight slots. On a
cellular link, 30–60 s is more realistic than the default.

Independently of this timer, everything still inflight is retransmitted
immediately after a successful CONNACK.

### `connect_timeout_ms` — default 30000

Deadline covering the whole handshake: transport connect plus CONNACK. On
expiry the session ends with `Error::ConnectTimeout`.

## Worked examples

### Constrained sensor node — telemetry only

QoS 0 publishes, one wildcard subscription for commands, tiny messages.

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

Well under 1 KB.

### Reliable sensor node

QoS 1 telemetry that must not be lost across a flaky link.

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

Roughly 3.5 KB, of which 1 KB is the retransmission store — that is the price of
the delivery guarantee, and it is visible rather than hidden in a heap.

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

Roughly 21 KB. Fine on anything with external RAM, and still entirely static.

## Measuring

Print it, or read the map file:

```cpp
static_assert(sizeof(mqtt::Client<MyConfig>) <= 4096, "MQTT client budget exceeded");
```

A `static_assert` in your build is the cheapest possible guard against someone
quietly raising a buffer size past what the target can afford.

To see where it goes, `tests/test_no_alloc.cpp` has a case that prints
`sizeof()` for three configurations; adding yours to it takes one line.
