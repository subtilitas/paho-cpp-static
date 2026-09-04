// SPDX-License-Identifier: MIT
//
// Compile-time capacity configuration.
//
// Every buffer, table and queue in this library is sized from one of these
// constants. Nothing grows at run time, so the client's entire footprint is a
// pure function of the config you instantiate it with and can be read off with
// sizeof() or a linker map.

#ifndef MQTT_CONFIG_HPP
#define MQTT_CONFIG_HPP

#include <cstddef>
#include <cstdint>

namespace mqtt {

/// Default capacities, tuned for a small sensor-class node.
///
/// Derive and override to resize. C++17 has no class-type non-type template
/// parameters, so configuration is passed as a *type*:
///
/// @code
/// struct MyConfig : mqtt::DefaultConfig
/// {
///     static constexpr size_t rx_buffer_size          = 2048;
///     static constexpr size_t max_inflight_out        = 8;
///     static constexpr size_t max_persisted_msg_size  = 256;
/// };
/// mqtt::Client<MyConfig> client{transport, clock};
/// @endcode
struct DefaultConfig
{
    /// Receive assembly buffer. Must hold the largest inbound packet whole,
    /// including its fixed header. Inbound packets larger than this are
    /// reported as Error::PacketTooLarge and drop the connection.
    static constexpr size_t rx_buffer_size = 1024;

    /// Outgoing byte FIFO. Holds serialized packets awaiting the transport.
    /// Must be at least as large as the biggest single packet you publish.
    static constexpr size_t tx_buffer_size = 1024;

    /// Longest topic name or topic filter, in bytes (UTF-8, no NUL).
    static constexpr size_t max_topic_len = 64;

    /// Longest client identifier.
    static constexpr size_t max_client_id_len = 24;

    /// Longest username; password is bounded by max_password_len.
    static constexpr size_t max_username_len = 32;
    static constexpr size_t max_password_len = 32;

    /// Concurrent outbound QoS 1 / QoS 2 messages awaiting acknowledgement.
    /// This is the MQTT "inflight window". Set to 0 to forbid QoS > 0
    /// publishing entirely.
    ///
    /// Zero means none for every capacity that accepts it, rather than the one
    /// slot the storage still holds. For the three that bound a call the
    /// application makes -- this one, max_subscriptions and max_pending_acks --
    /// that call is refused. max_inflight_in bounds what the broker sends, so
    /// there is no call to refuse; see the note there.
    ///
    /// The storage does not quite go away. A zero-length array is ill-formed,
    /// so every table still declares one element that nothing is ever put into.
    ///
    /// That residue only costs anything here, because an outbound slot carries
    /// a max_persisted_msg_size buffer -- so setting max_inflight_out to 0 on
    /// its own leaves one buffer of that size that nothing can reach. Set
    /// max_persisted_msg_size to 0 alongside it, which shrinks the remaining
    /// slot's buffer to a single byte. The worked sensor profile in
    /// docs/configuration.md sets both.
    static constexpr size_t max_inflight_out = 4;

    /// Bytes reserved per outbound slot to hold the serialized PUBLISH so it
    /// can be retransmitted with DUP set. Total cost is
    /// max_inflight_out * max_persisted_msg_size.
    static constexpr size_t max_persisted_msg_size = 256;

    /// Packet ids of inbound QoS 2 messages received but not yet released.
    /// Used for duplicate suppression between PUBREC and PUBCOMP. Set to 0 to
    /// track none, which makes every inbound QoS 2 message an overflow: it is
    /// counted by inbound_overflow_count() and left for the broker to
    /// retransmit rather than being acknowledged.
    ///
    /// Zero is worth setting only on a client that never subscribes above
    /// QoS 1, because it saves no memory: the one-element residue plus struct
    /// padding absorbs the difference, and sizeof(Client<Cfg>) is unchanged
    /// against the default of 4. What it does change is that inbound QoS 2
    /// becomes permanently undeliverable instead of transiently blocked, and
    /// silently so -- no call was made, so there is no error to return, and
    /// MQTT 3.1.1 has no way to refuse the QoS to the peer.
    /// inbound_overflow_count() is the only evidence.
    static constexpr size_t max_inflight_in = 4;

    /// Active subscriptions retained for redelivery on reconnect and for
    /// per-subscription callback dispatch. Set to 0 to refuse every
    /// subscribe() with Error::NoSubscriptionSlot.
    static constexpr size_t max_subscriptions = 8;

    /// SUBSCRIBE / UNSUBSCRIBE requests awaiting their ack. Set to 0 to refuse
    /// both with Error::NoPendingAckSlot -- a client that only publishes needs
    /// neither.
    static constexpr size_t max_pending_acks = 4;

    /// Topic filters permitted in a single SUBSCRIBE/UNSUBSCRIBE packet.
    static constexpr size_t max_topics_per_request = 4;

    /// Retransmit an unacknowledged QoS > 0 packet after this many
    /// milliseconds. 0 disables timed retransmission (retry only on reconnect).
    static constexpr uint32_t retry_interval_ms = 20000;

    /// Give up on the CONNECT handshake after this long.
    static constexpr uint32_t connect_timeout_ms = 30000;
};

/// Compile-time sanity checks, instantiated by Client. Failures here are
/// configuration bugs and are caught at build time rather than at 3am.
template <typename Cfg>
struct ConfigCheck
{
    static_assert(Cfg::rx_buffer_size >= 16,
                  "rx_buffer_size must hold at least a small packet");
    static_assert(Cfg::tx_buffer_size >= 16,
                  "tx_buffer_size must hold at least a CONNECT packet");
    static_assert(Cfg::max_topic_len >= 1, "max_topic_len must be non-zero");
    static_assert(Cfg::max_client_id_len >= 1, "max_client_id_len must be non-zero");
    static_assert(Cfg::max_topics_per_request >= 1, "max_topics_per_request must be non-zero");
    static_assert(Cfg::max_inflight_out == 0 || Cfg::max_persisted_msg_size >= 8,
                  "max_persisted_msg_size too small to hold a PUBLISH header");
    static_assert(Cfg::rx_buffer_size <= 268435460u,
                  "rx_buffer_size exceeds the MQTT maximum packet size");
};

}   // namespace mqtt

#endif   // MQTT_CONFIG_HPP
