// SPDX-License-Identifier: MIT
//
// A single-threaded, statically-allocated MQTT 3.1.1 client.
//
// Design contract
// ---------------
//  * No allocation after construction. Every buffer and table is a member array
//    sized by the Config type parameter. sizeof(Client<Cfg>) is the whole cost.
//  * No exceptions. Every failure path returns mqtt::Error.
//  * No threads, no mutexes, no OS headers. You call step() from your superloop
//    or RTOS task; the client never blocks and never calls back into itself.
//  * No hidden copies of your data. Received topics and payloads are views into
//    the receive buffer, valid only for the duration of the callback.
//
// Usage sketch
// ------------
// @code
// MyTransport transport;   // your TCP or TLS adapter
// MyClock     clock;       // your millisecond tick
// mqtt::Client<MyConfig> client{transport, clock};
//
// client.on_message([](const mqtt::Message& m) { handle(m); });
//
// mqtt::ConnectOptions opts;
// opts.client_id    = "sensor-07";
// opts.keep_alive_s = 30;
// client.connect(opts);
//
// for (;;)
// {
//     client.step();          // drives everything
//     do_other_work();
// }
// @endcode

#ifndef MQTT_CLIENT_HPP
#define MQTT_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <etl/array.h>
#include <etl/delegate.h>
#include <etl/span.h>
#include <etl/string.h>
#include <etl/string_view.h>
#include <etl/type_traits.h>
#include <etl/vector.h>

#include "mqtt/codec.hpp"
#include "mqtt/config.hpp"
#include "mqtt/error.hpp"
#include "mqtt/packet.hpp"
#include "mqtt/topic.hpp"
#include "mqtt/transport.hpp"
#include "mqtt/tx_queue.hpp"

namespace mqtt {

/// Connection lifecycle.
enum class State : uint8_t
{
    Idle,              ///< No connection and none being attempted.
    Connecting,        ///< Waiting for Transport::connect() to complete.
    AwaitingConnack,   ///< CONNECT sent, waiting for the broker's CONNACK.
    Connected,         ///< Session established.
    Disconnecting,     ///< DISCONNECT queued, draining the transmit queue.
};

const char* to_string(State s) noexcept;

namespace detail {
/// Zero-capacity arrays are not portable, so tables configured to hold nothing
/// still reserve one slot. The logical limit stays whatever the config says.
constexpr size_t at_least_one(size_t n) noexcept { return (n > 0) ? n : 1; }

/// A callback slot: a delegate that refuses a temporary callable.
///
/// A delegate stores a pointer to the callable rather than owning it, so one
/// bound to a callable that dies at the end of the statement is left pointing
/// at nothing. ETL deletes construction from an rvalue callable for that
/// reason, but its constraint exempts anything convertible to a plain function
/// pointer -- which a capture-less lambda is. That exemption reopens the hole
/// for exactly the inline lambda a caller is most likely to write:
///
/// @code
/// client.on_message([](const mqtt::Message& m) { handle(m); });   // rejected
///
/// static auto handler = [](const mqtt::Message& m) { handle(m); };
/// client.on_message(handler);                                     // fine
/// @endcode
///
/// Any lvalue the caller keeps alive binds as before -- a static, a member, or
/// a named local that outlives the client. Only the temporary is rejected, and
/// at compile time rather than by dangling at run time.
///
/// Composition rather than derivation because etl::delegate is final. It holds
/// one delegate and nothing else, so sizeof(Client<Cfg>) is unchanged.
template <typename Sig>
class Handler
{
public:
    /// The underlying delegate. Exposed because binding a member function goes
    /// through its create():
    ///
    /// @code
    /// using H = mqtt::Client<Cfg>::MessageHandler;
    /// client.on_message(H::Delegate::create<Sensor, &Sensor::on_message>(sensor));
    /// @endcode
    ///
    /// The object bound there must outlive the client, for the same reason a
    /// callable must.
    using Delegate = etl::delegate<Sig>;

private:
    /// A class type that is neither this slot nor the delegate it wraps --
    /// i.e. a user's lambda or functor. Excluding those two lets the copy
    /// constructor and the Delegate overload below win their own cases instead
    /// of tying with the constructor templates.
    ///
    /// Declared up here because a default template argument has to see it.
    template <typename F>
    static constexpr bool is_foreign_callable() noexcept
    {
        using T = etl::decay_t<F>;
        return etl::is_class<T>::value && !etl::is_same<T, Handler>::value &&
               !etl::is_same<T, Delegate>::value;
    }

public:
    /// An unset slot. call_if() on one does nothing.
    Handler() noexcept = default;

    /// Delegates built by Delegate::create() arrive as the delegate type.
    Handler(const Delegate& d) noexcept : d_(d) {}

    /// A named callable, whose lifetime the caller owns. F deduces the const
    /// on a const lvalue, so this one template covers both.
    template <typename F, typename = etl::enable_if_t<is_foreign_callable<F>()>>
    Handler(F& f) noexcept : d_(f)
    {
    }

    /// A temporary callable. Deleted rather than left out, so the diagnostic
    /// names this constructor and its comment.
    template <typename F, typename = etl::enable_if_t<!etl::is_lvalue_reference<F>::value &&
                                                      is_foreign_callable<F>()>>
    Handler(F&&) = delete;

    /// Invoke if a callable is bound; do nothing if not.
    template <typename... A>
    auto call_if(A&&... a) const
    {
        return d_.call_if(static_cast<A&&>(a)...);
    }

    /// Invoke unconditionally. Undefined if nothing is bound, exactly as for
    /// the underlying delegate -- check is_valid() first.
    template <typename... A>
    auto operator()(A&&... a) const
    {
        return d_(static_cast<A&&>(a)...);
    }

    bool is_valid() const noexcept { return d_.is_valid(); }

private:
    Delegate d_;
};
}   // namespace detail

/// A single-threaded MQTT 3.1.1 client that allocates nothing after
/// construction.
///
/// Every buffer and table is a member array sized from @p Cfg, so
/// `sizeof(Client<Cfg>)` is the complete RAM cost and an instance can live in
/// `.bss` on a target with no heap linked at all. Drive it by calling step()
/// regularly; it never blocks and never calls back into itself.
///
/// @tparam Cfg A configuration type, typically derived from DefaultConfig.
///             See config.hpp for the capacities it must supply.
///
/// @code
/// struct MyConfig : mqtt::DefaultConfig
/// {
///     static constexpr size_t rx_buffer_size = 2048;
/// };
///
/// static MyTransport            transport;
/// static MyClock                clock;
/// static mqtt::Client<MyConfig> client{transport, clock};
/// @endcode
// Deriving from ConfigCheck is what fires its static_asserts: a base class has
// to be a complete type, which instantiates the template. It is a base rather
// than a member because an empty *member* still costs a byte plus alignment
// padding -- eight bytes here -- whereas an empty base costs nothing under the
// empty base optimisation, and sizeof(Client<Cfg>) is a documented promise.
// Private, because it is a build-time assertion and not part of the interface.
template <typename Cfg = DefaultConfig>
class Client : private ConfigCheck<Cfg>
{
public:
    /// Handler for a received message. The message's topic and payload are
    /// views into the receive buffer and are valid only for the call.
    ///
    /// Handlers run inside step(), while the subscription table is being walked.
    /// What a handler may do to the client:
    ///
    /// - publish(), and every introspection accessor: fine.
    /// - disconnect() and abort(): fine. The session ends, and the rest of
    ///   this step() is abandoned.
    /// - subscribe() and unsubscribe(): no. They mutate the table being
    ///   walked. Set a flag and act on it after step() returns.
    /// - step(): refused, returning Error::Reentrant. It would re-drain the
    ///   same bytes and re-enter this handler, without bound.
    ///
    /// Ending the session from a handler is supported rather than merely
    /// tolerated: abort() exists so a handler can drop the connection and let
    /// the broker publish the will, which is what a handler does on receiving a
    /// shutdown command.
    using MessageHandler = detail::Handler<void(const Message&)>;
    /// Handler invoked once the broker has accepted the CONNECT.
    using ConnectHandler = detail::Handler<void(const ConnackInfo&)>;
    /// Handler invoked when a session ends, with the reason it ended.
    using DisconnectHandler = detail::Handler<void(Error)>;
    /// Handler invoked when a QoS 1 or QoS 2 publish completes, given its
    /// packet id.
    using DeliveryHandler = detail::Handler<void(uint16_t)>;
    /// Handler invoked on SUBACK with the packet id and the broker's granted
    /// QoS for each requested filter.
    using SubackHandler = detail::Handler<void(uint16_t, etl::span<const uint8_t>)>;

    /// Construct a client over a transport and a clock.
    ///
    /// Both are borrowed, not owned, and must outlive the client. Construction
    /// performs no I/O and no allocation.
    ///
    /// @param transport The byte stream to run MQTT over. See transport.hpp.
    /// @param clock     A monotonic millisecond source; it may wrap.
    Client(Transport& transport, Clock& clock) noexcept : transport_(transport), clock_(clock)
    {
    }

    // Neither copyable nor movable: the client borrows a transport and a clock
    // by reference and is expected to live in .bss for the life of the program.
    // Moving one mid-session would leave the transport talking to a corpse.
    ~Client() = default;

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&)                 = delete;
    Client& operator=(Client&&)      = delete;

    //--------------------------------------------------------------------------
    // Callbacks. All are optional and all are invoked from within step().
    //--------------------------------------------------------------------------

    /// Fallback handler for messages no per-subscription handler matched.
    ///
    /// Dispatch is by filter match, not by "first match wins": a message that
    /// matches several subscriptions is delivered to each of their handlers, in
    /// table order -- until one of them ends the session, after which the rest
    /// are not called.
    /// Subscribing to both "a/#" and "a/b" therefore sees "a/b" twice. This
    /// fallback runs only when no per-subscription handler matched at all.
    void on_message(MessageHandler h) noexcept { on_message_ = h; }
    /// Invoked once the broker accepts the CONNECT.
    void on_connect(ConnectHandler h) noexcept { on_connect_ = h; }
    /// Invoked whenever an established or attempted session ends, with the reason.
    void on_disconnect(DisconnectHandler h) noexcept { on_disconnect_ = h; }
    /// Invoked when a QoS 1 or QoS 2 publish completes its handshake.
    void on_delivery_complete(DeliveryHandler h) noexcept { on_delivery_ = h; }
    /// Invoked on SUBACK with the broker's granted QoS per requested filter.
    void on_suback(SubackHandler h) noexcept { on_suback_ = h; }

    //--------------------------------------------------------------------------
    // Lifecycle
    //--------------------------------------------------------------------------

    /// Begin connecting. Non-blocking: the CONNECT packet is serialized
    /// immediately, so `opts` and everything it points at may be destroyed as
    /// soon as this returns. Progress happens in step().
    Error connect(const ConnectOptions& opts) noexcept
    {
        if (state_ != State::Idle)
            return Error::AlreadyConnected;

        if (opts.client_id.size() > Cfg::max_client_id_len)
            return Error::InvalidArgument;
        if (opts.username.size() > Cfg::max_username_len)
            return Error::InvalidArgument;
        if (opts.password.size() > Cfg::max_password_len)
            return Error::InvalidArgument;
        if (opts.will.valid() && !is_valid_topic_name(opts.will.topic))
            return Error::InvalidArgument;

        // MQTT-3.1.3-8: a zero-length client id asks the broker to assign one,
        // which it can only do for a session it is not expected to remember.
        // Catch it here rather than spending a round trip to be told.
        if (opts.client_id.empty() && !opts.clean_session)
            return Error::InvalidArgument;

        const Result<uint32_t> rl = codec::connect_remaining_length(opts);
        if (!rl.ok())
            return rl.error();

        tx_.clear();
        rx_len_ = 0;

        // A clean session tells the broker to discard prior state, so we drop
        // ours too rather than retransmitting into a session that no longer
        // exists.
        if (opts.clean_session)
            reset_session_state();

        const Error e = enqueue_sized(packet_size(rl.value()), [&](etl::span<uint8_t> out) {
            return codec::encode_connect(out, opts);
        });
        if (e != Error::Ok)
            return e;

        clean_session_ = opts.clean_session;
        keep_alive_ms_ = static_cast<uint32_t>(opts.keep_alive_s) * 1000u;
        connack_       = ConnackInfo{};
        last_error_    = Error::Ok;

        const uint32_t now  = clock_.now_ms();
        connect_started_ms_ = now;
        last_sent_ms_       = now;
        last_received_ms_   = now;
        ping_outstanding_   = false;

        state_ = State::Connecting;
        return Error::Ok;
    }

    /// Queue a DISCONNECT and close once it has gone out. Graceful: the broker
    /// will not publish the will message.
    Error disconnect() noexcept
    {
        if (state_ == State::Idle)
            return Error::NotConnected;

        if (state_ == State::Connected)
        {
            const Error e = enqueue_sized(packet_size(0), [](etl::span<uint8_t> out) {
                return codec::encode_empty(out, PacketType::Disconnect);
            });
            if (e == Error::Ok)
            {
                state_ = State::Disconnecting;
                return Error::Ok;
            }
            // If the queue is wedged, fall through to a hard close rather than
            // leaving the caller stuck.
        }

        shutdown(Error::Ok);
        return Error::Ok;
    }

    /// Drop the connection immediately without a DISCONNECT. The broker will
    /// publish the will message, which is the point.
    void abort() noexcept { shutdown(Error::TransportClosed); }

    //--------------------------------------------------------------------------
    // Publish / subscribe
    //--------------------------------------------------------------------------

    /// Publish a message.
    ///
    /// QoS 0 is serialized straight into the transmit queue. QoS 1 and 2 also
    /// take an inflight slot and keep a serialized copy for retransmission, so
    /// they fail with Error::NoInflightSlot when the window is full and
    /// Error::PayloadTooLarge when the packet exceeds max_persisted_msg_size.
    ///
    /// @param out_packet_id Optional; receives the assigned id for QoS > 0.
    Error publish(etl::string_view topic, etl::span<const uint8_t> payload,
                  QoS qos = QoS::AtMostOnce, bool retain = false,
                  uint16_t* out_packet_id = nullptr) noexcept
    {
        if (state_ != State::Connected)
            return Error::NotConnected;
        if (!is_valid_topic_name(topic))
            return Error::InvalidArgument;
        if (topic.size() > Cfg::max_topic_len)
            return Error::TopicTooLong;

        if (qos == QoS::AtMostOnce)
        {
            const Result<uint32_t> rl =
                codec::publish_remaining_length(topic, payload.size(), qos);
            if (!rl.ok())
                return rl.error();

            return enqueue_sized(packet_size(rl.value()), [&](etl::span<uint8_t> out) {
                return codec::encode_publish(out, topic, payload, qos, retain, false, 0);
            });
        }

        if (Cfg::max_inflight_out == 0)
            return Error::NotSupported;

        OutboundSlot* slot = free_outbound_slot();
        if (slot == nullptr)
            return Error::NoInflightSlot;

        const uint16_t id = alloc_packet_id();
        if (id == 0)
            return Error::NoInflightSlot;

        // Serialize into the slot's retransmission buffer first; if it does not
        // fit there we must reject the publish outright, because we could not
        // honour the delivery guarantee later.
        const Result<size_t> n =
            codec::encode_publish(etl::span<uint8_t>(slot->packet.data(), slot->packet.size()),
                                  topic, payload, qos, retain, false, id);
        if (!n.ok())
            return (n.error() == Error::BufferTooSmall) ? Error::PayloadTooLarge : n.error();

        const Error e = enqueue(etl::span<const uint8_t>(slot->packet.data(), n.value()));
        if (e != Error::Ok)
            return e;   // slot still free: we never marked it in use

        slot->packet_id    = id;
        slot->packet_len   = static_cast<uint16_t>(n.value());
        slot->qos          = qos;
        slot->phase        = (qos == QoS::AtLeastOnce) ? Phase::WaitPuback : Phase::WaitPubrec;
        slot->last_sent_ms = clock_.now_ms();

        if (out_packet_id != nullptr)
            *out_packet_id = id;

        return Error::Ok;
    }

    /// Convenience overload for text payloads.
    Error publish(etl::string_view topic, etl::string_view payload, QoS qos = QoS::AtMostOnce,
                  bool retain = false, uint16_t* out_packet_id = nullptr) noexcept
    {
        return publish(topic,
                       etl::span<const uint8_t>(
                           reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
                       qos, retain, out_packet_id);
    }

    /// Subscribe to one filter, optionally with a dedicated handler.
    ///
    /// The filter is copied into the subscription table, so the caller's string
    /// need not outlive the call. The subscription is retained across
    /// reconnects and re-sent automatically.
    Error subscribe(etl::string_view filter, QoS qos = QoS::AtMostOnce,
                    MessageHandler handler       = MessageHandler(),
                    uint16_t*      out_packet_id = nullptr) noexcept
    {
        const TopicSubscription one{filter, qos};
        return subscribe(etl::span<const TopicSubscription>(&one, 1), handler, out_packet_id);
    }

    /// Subscribe to several filters in one SUBSCRIBE packet. All entries share
    /// `handler`; pass a default-constructed handler to route them to the
    /// on_message fallback instead.
    Error subscribe(etl::span<const TopicSubscription> subs,
                    MessageHandler                     handler       = MessageHandler(),
                    uint16_t*                          out_packet_id = nullptr) noexcept
    {
        if (state_ != State::Connected)
            return Error::NotConnected;
        if (subs.empty() || subs.size() > Cfg::max_topics_per_request)
            return Error::InvalidArgument;

        for (const TopicSubscription& s : subs)
        {
            if (!is_valid_filter(s.filter))
                return Error::InvalidArgument;
            if (s.filter.size() > Cfg::max_topic_len)
                return Error::TopicTooLong;
        }

        if (pending_.full())
            return Error::NoPendingAckSlot;

        // Reserve subscription-table space before touching the wire, so a
        // half-registered subscription is impossible.
        size_t new_entries = 0;
        for (const TopicSubscription& s : subs)
        {
            if (find_subscription(s.filter) == nullptr)
                ++new_entries;
        }
        if (subscriptions_.size() + new_entries > Cfg::max_subscriptions)
            return Error::NoSubscriptionSlot;

        const Result<uint32_t> rl = codec::subscribe_remaining_length(subs);
        if (!rl.ok())
            return rl.error();

        const uint16_t id = alloc_packet_id();
        if (id == 0)
            return Error::NoPendingAckSlot;

        const Error e = enqueue_sized(packet_size(rl.value()), [&](etl::span<uint8_t> out) {
            return codec::encode_subscribe(out, id, subs);
        });
        if (e != Error::Ok)
            return e;

        PendingAck ack;
        ack.packet_id = id;
        ack.expected  = PacketType::Suback;
        ack.sent_ms   = clock_.now_ms();

        for (const TopicSubscription& s : subs)
        {
            Subscription* existing = find_subscription(s.filter);
            if (existing != nullptr)
            {
                existing->requested_qos = s.qos;
                existing->handler       = handler;
                existing->needs_resub   = false;
                ack.subs.push_back(existing->sub_id);
            }
            else
            {
                Subscription entry;
                entry.sub_id = alloc_sub_id();
                entry.filter.assign(s.filter.data(), s.filter.size());
                entry.requested_qos = s.qos;
                entry.granted_qos   = 0;
                entry.handler       = handler;
                entry.needs_resub   = false;
                subscriptions_.push_back(entry);
                ack.subs.push_back(entry.sub_id);
            }
        }

        pending_.push_back(ack);

        if (out_packet_id != nullptr)
            *out_packet_id = id;

        return Error::Ok;
    }

    /// Unsubscribe from one filter. The table entry is removed on UNSUBACK.
    Error unsubscribe(etl::string_view filter, uint16_t* out_packet_id = nullptr) noexcept
    {
        return unsubscribe(etl::span<const etl::string_view>(&filter, 1), out_packet_id);
    }

    Error unsubscribe(etl::span<const etl::string_view> filters,
                      uint16_t*                         out_packet_id = nullptr) noexcept
    {
        if (state_ != State::Connected)
            return Error::NotConnected;
        if (filters.empty() || filters.size() > Cfg::max_topics_per_request)
            return Error::InvalidArgument;
        if (pending_.full())
            return Error::NoPendingAckSlot;

        const Result<uint32_t> rl = codec::unsubscribe_remaining_length(filters);
        if (!rl.ok())
            return rl.error();

        const uint16_t id = alloc_packet_id();
        if (id == 0)
            return Error::NoPendingAckSlot;

        const Error e = enqueue_sized(packet_size(rl.value()), [&](etl::span<uint8_t> out) {
            return codec::encode_unsubscribe(out, id, filters);
        });
        if (e != Error::Ok)
            return e;

        PendingAck ack;
        ack.packet_id = id;
        ack.expected  = PacketType::Unsuback;
        ack.sent_ms   = clock_.now_ms();

        for (const etl::string_view& f : filters)
        {
            Subscription* s = find_subscription(f);
            if (s != nullptr)
                ack.subs.push_back(s->sub_id);
        }

        pending_.push_back(ack);

        if (out_packet_id != nullptr)
            *out_packet_id = id;

        return Error::Ok;
    }

    //--------------------------------------------------------------------------
    // The pump
    //--------------------------------------------------------------------------

    /// Advance the client. Call this regularly -- at least several times per
    /// keep-alive interval. Never blocks.
    ///
    /// Returns Error::Ok when everything is fine (including when there was
    /// simply nothing to do). Any other value means the session has just ended
    /// and the same value was passed to the on_disconnect handler -- with one
    /// exception, Error::Reentrant, which means only that this call came from
    /// inside a handler and did nothing. The session is untouched and no
    /// handler was notified, so `if (step() != Error::Ok) reconnect();` would
    /// tear down a healthy connection; a handler that calls step() should check
    /// for that code, and an application that never does cannot receive it.
    ///
    /// Refused from a handler, rather than forbidden there: the nested call
    /// returns Error::Reentrant and does nothing, and the session carries on.
    /// A handler runs inside step(), on the buffer step() is in the middle of
    /// draining, so a nested call would re-drain the same bytes, re-enter the
    /// same handler and recurse without bound -- breaking the bounded-stack
    /// guarantee this library measures on target in CI.
    Error step() noexcept
    {
        if (in_step_)
            return Error::Reentrant;

        in_step_      = true;
        const Error e = step_once();
        in_step_      = false;
        return e;
    }

private:
    Error step_once() noexcept
    {
        if (state_ == State::Idle)
            return Error::Ok;

        if (state_ == State::Connecting)
        {
            const Error e = transport_.connect();
            if (e == Error::WouldBlock)
            {
                if (elapsed_ms(clock_.now_ms(), connect_started_ms_) > Cfg::connect_timeout_ms)
                    return shutdown(Error::ConnectTimeout);
                return Error::Ok;
            }
            if (e != Error::Ok)
                return shutdown(e);

            state_        = State::AwaitingConnack;
            last_sent_ms_ = clock_.now_ms();
        }

        // Drain whatever is already queued before looking at the wire.
        {
            const Error e = pump_tx();
            if (e != Error::Ok)
                return shutdown(e);
        }

        // A graceful disconnect completes once the DISCONNECT has drained.
        if (state_ == State::Disconnecting && tx_.empty())
            return shutdown(Error::Ok);

        {
            const Error e = pump_rx();
            if (e != Error::Ok)
                return shutdown(e);
        }

        // A handler may have ended this session and started another one --
        // abort() then connect(), which is how an application reacts to a
        // "reconfigure and reconnect" message. The new CONNECT is queued but
        // the transport is closed, and transport_.connect() runs only at the
        // top of this function, so everything below would write to a torn-down
        // transport. Leave the rest to the next step(), which starts there.
        //
        // Only connect() sets Connecting, and the block at the top already
        // dealt with the case where this pass began that way.
        if (state_ == State::Connecting)
            return Error::Ok;

        if (state_ == State::AwaitingConnack)
        {
            if (elapsed_ms(clock_.now_ms(), connect_started_ms_) > Cfg::connect_timeout_ms)
                return shutdown(Error::ConnectTimeout);
            return Error::Ok;
        }

        if (state_ == State::Connected)
        {
            const Error e = pump_keep_alive();
            if (e != Error::Ok)
                return shutdown(e);

            pump_resubscribe();
            pump_retransmit();
        }

        // Flush again. Receiving generates acknowledgements, and the timers
        // generate pings and retransmissions; without this second pass all of
        // them would sit in the queue until the next step(), adding a full
        // scheduling period of latency to every ack the protocol depends on.
        {
            const Error e = pump_tx();
            if (e != Error::Ok)
                return shutdown(e);
        }

        return Error::Ok;
    }

public:
    //--------------------------------------------------------------------------
    // Introspection
    //--------------------------------------------------------------------------

    State state() const noexcept { return state_; }
    bool  is_connected() const noexcept { return state_ == State::Connected; }

    /// Reason the last session ended, or the CONNECT failure reason.
    Error last_error() const noexcept { return last_error_; }

    /// The broker's CONNACK, valid once on_connect has fired. When
    /// last_error() is Error::ConnectionRefused, connack().code says why.
    const ConnackInfo& connack() const noexcept { return connack_; }

    /// Outbound QoS > 0 messages currently awaiting acknowledgement.
    size_t inflight_count() const noexcept
    {
        size_t n = 0;
        for (size_t i = 0; i < Cfg::max_inflight_out; ++i)
        {
            if (outbound_[i].phase != Phase::Free)
                ++n;
        }
        return n;
    }

    size_t subscription_count() const noexcept { return subscriptions_.size(); }

    /// Milliseconds since the transport last produced any bytes. Useful as a
    /// liveness signal alongside the keep-alive machinery.
    uint32_t ms_since_last_receive() const noexcept
    {
        return elapsed_ms(clock_.now_ms(), last_received_ms_);
    }

    /// Count of inbound QoS 2 messages dropped because the tracking table was
    /// full. A non-zero value means max_inflight_in is undersized for the
    /// broker's delivery rate.
    uint32_t inbound_overflow_count() const noexcept { return inbound_overflow_count_; }

    /// Count of acknowledgements deferred because the transmit queue was full.
    /// The protocol recovers on its own -- the peer retransmits and the ack is
    /// sent then -- so this is a tuning signal rather than an error: a
    /// persistently rising value means tx_buffer_size is undersized for the
    /// traffic, or step() is not being called often enough to drain it.
    uint32_t tx_backpressure_count() const noexcept { return tx_backpressure_count_; }

    /// Bytes serialized but not yet accepted by the transport. Useful as a
    /// backpressure signal before publishing more.
    size_t tx_pending() const noexcept { return tx_.pending(); }

private:
    //--------------------------------------------------------------------------
    // Internal tables
    //--------------------------------------------------------------------------

    enum class Phase : uint8_t
    {
        Free,
        WaitPuback,    ///< QoS 1: PUBLISH sent
        WaitPubrec,    ///< QoS 2: PUBLISH sent
        WaitPubcomp,   ///< QoS 2: PUBREL sent
    };

    static constexpr size_t kOutSlots    = detail::at_least_one(Cfg::max_inflight_out);
    static constexpr size_t kPersistSize = detail::at_least_one(Cfg::max_persisted_msg_size);
    static constexpr size_t kInSlots     = detail::at_least_one(Cfg::max_inflight_in);
    static constexpr size_t kSubSlots    = detail::at_least_one(Cfg::max_subscriptions);
    static constexpr size_t kAckSlots    = detail::at_least_one(Cfg::max_pending_acks);

    /// One outbound QoS > 0 message. `packet` holds whatever must be
    /// retransmitted right now: the PUBLISH before PUBREC arrives, then the
    /// much smaller PUBREL. Reusing the buffer keeps the footprint flat.
    struct OutboundSlot
    {
        etl::array<uint8_t, kPersistSize> packet{};
        uint16_t                          packet_id    = 0;
        uint16_t                          packet_len   = 0;
        uint32_t                          last_sent_ms = 0;
        QoS                               qos          = QoS::AtMostOnce;
        Phase                             phase        = Phase::Free;
    };

    struct Subscription
    {
        etl::string<Cfg::max_topic_len> filter;
        MessageHandler                  handler;
        /// Identity that survives table compaction. A SUBACK that refuses a
        /// filter erases an entry and shifts every entry after it, so a
        /// position recorded when the request was sent is worthless by the
        /// time the ack arrives -- including positions recorded by a *different*
        /// request that is still in flight. Acks therefore name subscriptions
        /// by id and look them up, never by index.
        uint16_t sub_id        = 0;
        QoS      requested_qos = QoS::AtMostOnce;
        uint8_t  granted_qos   = 0;
        bool     needs_resub   = false;
    };

    struct PendingAck
    {
        /// sub_ids of the subscriptions this request covers, in wire order.
        etl::vector<uint16_t, Cfg::max_topics_per_request> subs;
        uint32_t                                           sent_ms   = 0;
        uint16_t                                           packet_id = 0;
        PacketType                                         expected  = PacketType::Reserved;
    };

    static constexpr size_t packet_size(uint32_t remaining_length) noexcept
    {
        return 1u + vbi_size_c(remaining_length) + remaining_length;
    }

    static constexpr size_t vbi_size_c(uint32_t v) noexcept
    {
        return (v < 128u) ? 1u : (v < 16384u) ? 2u : (v < 2097152u) ? 3u : 4u;
    }

    //--------------------------------------------------------------------------
    // Transmit path
    //--------------------------------------------------------------------------

    Error enqueue(etl::span<const uint8_t> bytes) noexcept
    {
        if (!tx_.reserve(bytes.size()))
            return Error::TxQueueFull;

        etl::span<uint8_t> dst = tx_.write_span();
        std::memcpy(dst.data(), bytes.data(), bytes.size());
        tx_.commit(bytes.size());
        return Error::Ok;
    }

    /// Reserve `size` bytes and let `encode` write directly into the queue,
    /// avoiding a staging buffer and a copy.
    template <typename EncodeFn>
    Error enqueue_sized(size_t size, EncodeFn&& encode) noexcept
    {
        if (size > Cfg::tx_buffer_size)
            return Error::BufferTooSmall;
        if (!tx_.reserve(size))
            return Error::TxQueueFull;

        const Result<size_t> n = encode(tx_.write_span().first(size));
        if (!n.ok())
            return n.error();

        tx_.commit(n.value());
        return Error::Ok;
    }

    Error pump_tx() noexcept
    {
        while (!tx_.empty())
        {
            const etl::span<const uint8_t> out     = tx_.peek();
            size_t                         written = 0;

            const Error e = transport_.send(out, written);
            if (e == Error::WouldBlock)
                return Error::Ok;   // back off; try again next step()
            if (e != Error::Ok)
                return e;

            if (written == 0)
                return Error::Ok;   // no progress, do not spin

            tx_.consume(written);
            last_sent_ms_ = clock_.now_ms();
        }
        return Error::Ok;
    }

    //--------------------------------------------------------------------------
    // Receive path
    //--------------------------------------------------------------------------

    /// Upper bound on recv() calls per step(), so one chatty broker cannot make
    /// step() run unboundedly long and starve the rest of the caller's loop.
    static constexpr int kMaxRecvRoundsPerStep = 8;

    Error pump_rx() noexcept
    {
        for (int round = 0; round < kMaxRecvRoundsPerStep; ++round)
        {
            if (rx_len_ < Cfg::rx_buffer_size)
            {
                size_t      read = 0;
                const Error e    = transport_.recv(
                    etl::span<uint8_t>(rx_.data() + rx_len_, Cfg::rx_buffer_size - rx_len_),
                    read);

                if (e == Error::WouldBlock)
                {
                    // Nothing new on the wire; still drain whatever we hold.
                    return drain_rx();
                }
                if (e != Error::Ok)
                    return e;

                if (read == 0)
                    return drain_rx();

                rx_len_ += read;
                last_received_ms_ = clock_.now_ms();
            }

            const Error e = drain_rx();
            if (e != Error::Ok)
                return e;

            // drain_rx succeeded, but a handler may have replaced the session
            // while it ran: abort() followed by connect() leaves Connecting
            // with the transport closed and a new CONNECT queued. Another
            // recv() here would fail on that closed transport and be reported
            // as this session failing, which would tear the new one down
            // before step() had a chance to establish it.
            //
            // A handler that ends the session without starting another leaves
            // Idle, and drain_rx has already returned the reason above.
            if (state_ == State::Connecting)
                return Error::Ok;

            // If the buffer is still full after draining, the pending packet
            // cannot fit and never will.
            if (rx_len_ == Cfg::rx_buffer_size)
                return Error::PacketTooLarge;
        }
        return Error::Ok;   // round budget spent; resume on the next step()
    }

    /// Consume every complete packet sitting in the receive buffer.
    Error drain_rx() noexcept
    {
        for (;;)
        {
            if (rx_len_ == 0)
                return Error::Ok;

            const Result<codec::PacketPeek> peek =
                codec::peek_header(etl::span<const uint8_t>(rx_.data(), rx_len_));

            if (!peek.ok())
            {
                if (peek.error() == Error::Incomplete)
                    return Error::Ok;   // fixed header still arriving
                return peek.error();
            }

            const codec::PacketPeek& p = peek.value();

            if (p.total_bytes > Cfg::rx_buffer_size)
                return Error::PacketTooLarge;

            if (rx_len_ < p.total_bytes)
                return Error::Ok;   // body still arriving

            const etl::span<const uint8_t> body(rx_.data() + p.header_bytes,
                                                p.header.remaining_length);

            const Error e = handle_packet(p.header, body);

            // handle_packet runs application callbacks, and a callback can
            // empty the receive buffer underneath this frame: abort() and a
            // disconnect() that cannot queue both reach shutdown(), which sets
            // rx_len_ to zero, and a reentrant step() drains the buffer itself.
            //
            // p.total_bytes was measured before that happened, so the
            // subtraction below would wrap -- it is size_t -- and hand memmove
            // a length near SIZE_MAX.
            //
            // Detect it by the only thing that can be true afterwards, and stop
            // rather than clamp: the bytes still held belong to a session that
            // has ended, or to an inner drain that already dealt with them.
            // There is nothing left here worth shifting.
            if (rx_len_ < p.total_bytes)
            {
                rx_len_ = 0;
                return (e != Error::Ok) ? e : last_error_;
            }

            // Shift the remainder down before reacting to any error, so the
            // buffer is always in a consistent state.
            const size_t leftover = rx_len_ - p.total_bytes;
            if (leftover > 0)
                std::memmove(rx_.data(), rx_.data() + p.total_bytes, leftover);
            rx_len_ = leftover;

            if (e != Error::Ok)
                return e;
        }
    }

    Error handle_packet(const FixedHeader& h, etl::span<const uint8_t> body) noexcept
    {
        switch (h.type)
        {
            case PacketType::Connack: return handle_connack(body);
            case PacketType::Publish: return handle_publish(h, body);
            case PacketType::Puback: return handle_puback(body);
            case PacketType::Pubrec: return handle_pubrec(body);
            case PacketType::Pubrel: return handle_pubrel(body);
            case PacketType::Pubcomp: return handle_pubcomp(body);
            case PacketType::Suback: return handle_suback(body);
            case PacketType::Unsuback: return handle_unsuback(body);
            case PacketType::Pingresp:
                if (!body.empty())
                    return Error::MalformedPacket;
                ping_outstanding_ = false;
                return Error::Ok;

            // Packets a server must never send to a client.
            case PacketType::Connect:
            case PacketType::Subscribe:
            case PacketType::Unsubscribe:
            case PacketType::Pingreq:
            case PacketType::Disconnect:
            case PacketType::Reserved:
            default: return Error::ProtocolViolation;
        }
    }

    Error handle_connack(etl::span<const uint8_t> body) noexcept
    {
        if (state_ != State::AwaitingConnack)
            return Error::ProtocolViolation;

        const Result<ConnackInfo> info = codec::decode_connack(body);
        if (!info.ok())
            return info.error();

        connack_ = info.value();

        if (connack_.code != ConnackCode::Accepted)
            return Error::ConnectionRefused;

        state_            = State::Connected;
        ping_outstanding_ = false;

        // Anything still inflight was never acknowledged, so push it out now
        // rather than waiting for the retry timer to come round.
        retransmit_now_ = true;

        // If the broker has no record of our session, anything we thought was
        // established is gone: re-subscribe from our own table.
        if (!connack_.session_present)
        {
            for (Subscription& s : subscriptions_)
            {
                s.needs_resub = true;
            }
        }

        on_connect_.call_if(connack_);
        return Error::Ok;
    }

    Error handle_publish(const FixedHeader& h, etl::span<const uint8_t> body) noexcept
    {
        if (state_ != State::Connected)
            return Error::ProtocolViolation;

        const Result<Message> msg = codec::decode_publish(h, body);
        if (!msg.ok())
            return msg.error();

        const Message& m = msg.value();

        switch (m.qos)
        {
            case QoS::AtMostOnce: dispatch(m); return Error::Ok;

            case QoS::AtLeastOnce:
            {
                // Acknowledge first, deliver second. If the transmit queue has
                // no room the message is left both unacknowledged and
                // undelivered, and the broker retransmits it -- which is
                // precisely the QoS 1 contract. Delivering first and then
                // failing to ack would duplicate the message instead.
                if (send_ack(PacketType::Puback, m.packet_id) != Error::Ok)
                {
                    ++tx_backpressure_count_;
                    return Error::Ok;
                }
                dispatch(m);
                return Error::Ok;
            }

            case QoS::ExactlyOnce:
            {
                // Deliver once. The id is remembered until PUBREL so a
                // retransmitted PUBLISH is acknowledged but not re-delivered.
                if (inbound_contains(m.packet_id))
                {
                    if (send_ack(PacketType::Pubrec, m.packet_id) != Error::Ok)
                        ++tx_backpressure_count_;
                    return Error::Ok;
                }

                if (inbound_ids_.full())
                {
                    // No room to track it, so exactly-once cannot be honoured.
                    // Staying silent is the correct move: the broker still owns
                    // the message and will retransmit it. Acknowledging instead
                    // would let it drop a message we never delivered, and
                    // dropping the connection would hand a peer a trivial way
                    // to knock us offline.
                    ++inbound_overflow_count_;
                    return Error::Ok;
                }

                // Same ordering argument as QoS 1, and the same remedy: with no
                // room for the PUBREC we neither track nor deliver, so the
                // retransmission arrives at a client that has not seen it.
                if (send_ack(PacketType::Pubrec, m.packet_id) != Error::Ok)
                {
                    ++tx_backpressure_count_;
                    return Error::Ok;
                }

                inbound_ids_.push_back(m.packet_id);
                dispatch(m);
                return Error::Ok;
            }
        }
        return Error::ProtocolViolation;
    }

    Error handle_puback(etl::span<const uint8_t> body) noexcept
    {
        const Result<uint16_t> id = codec::decode_packet_id(body);
        if (!id.ok())
            return id.error();

        OutboundSlot* slot = find_outbound(id.value());
        if (slot == nullptr || slot->phase != Phase::WaitPuback)
            return Error::Ok;   // unknown or already completed: tolerate quietly

        release_slot(*slot);
        on_delivery_.call_if(id.value());
        return Error::Ok;
    }

    Error handle_pubrec(etl::span<const uint8_t> body) noexcept
    {
        const Result<uint16_t> id = codec::decode_packet_id(body);
        if (!id.ok())
            return id.error();

        OutboundSlot* slot = find_outbound(id.value());
        if (slot == nullptr)
            return Error::Ok;

        if (slot->phase == Phase::WaitPubrec)
        {
            // From here on the retransmission unit is PUBREL, not the original
            // PUBLISH, so overwrite the stored packet with it.
            const Result<size_t> n =
                codec::encode_ack(etl::span<uint8_t>(slot->packet.data(), slot->packet.size()),
                                  PacketType::Pubrel, id.value());
            if (!n.ok())
                return n.error();

            slot->packet_len = static_cast<uint16_t>(n.value());
            slot->phase      = Phase::WaitPubcomp;

            // A full queue is not fatal here either: the PUBREL is stored in
            // the slot, so pump_retransmit() will put it out once there is
            // room. Only stamp the clock on success, so the retry timer starts
            // from the last actual transmission rather than from this attempt.
            if (enqueue(etl::span<const uint8_t>(slot->packet.data(), slot->packet_len)) ==
                Error::Ok)
                slot->last_sent_ms = clock_.now_ms();
            else
                ++tx_backpressure_count_;

            return Error::Ok;
        }

        if (slot->phase == Phase::WaitPubcomp)
        {
            // Duplicate PUBREC; re-send PUBREL and move on.
            if (enqueue(etl::span<const uint8_t>(slot->packet.data(), slot->packet_len)) ==
                Error::Ok)
                slot->last_sent_ms = clock_.now_ms();
            else
                ++tx_backpressure_count_;
        }

        return Error::Ok;
    }

    Error handle_pubrel(etl::span<const uint8_t> body) noexcept
    {
        const Result<uint16_t> id = codec::decode_packet_id(body);
        if (!id.ok())
            return id.error();

        // Release the tracking slot only once the PUBCOMP is queued. With no
        // room the id stays held, so a retransmitted PUBREL still finds it and
        // is answered on the next attempt.
        const Error e = send_ack(PacketType::Pubcomp, id.value());
        if (e != Error::Ok)
        {
            ++tx_backpressure_count_;
            return Error::Ok;
        }

        inbound_remove(id.value());
        return Error::Ok;
    }

    Error handle_pubcomp(etl::span<const uint8_t> body) noexcept
    {
        const Result<uint16_t> id = codec::decode_packet_id(body);
        if (!id.ok())
            return id.error();

        OutboundSlot* slot = find_outbound(id.value());
        if (slot == nullptr || slot->phase != Phase::WaitPubcomp)
            return Error::Ok;

        release_slot(*slot);
        on_delivery_.call_if(id.value());
        return Error::Ok;
    }

    Error handle_suback(etl::span<const uint8_t> body) noexcept
    {
        const Result<codec::SubackView> sub = codec::decode_suback(body);
        if (!sub.ok())
            return sub.error();

        const codec::SubackView& v = sub.value();

        PendingAck* ack = find_pending(v.packet_id, PacketType::Suback);
        if (ack == nullptr)
            return Error::Ok;   // stale ack, ignore

        if (v.return_codes.size() != ack->subs.size())
            return Error::ProtocolViolation;

        // Order does not matter: each entry is named by id, so erasing one
        // cannot disturb the lookup of any other.
        for (size_t i = 0; i < v.return_codes.size(); ++i)
        {
            const uint8_t rc = v.return_codes[i];
            if (rc == kSubackFailure)
            {
                erase_subscription(ack->subs[i]);
            }
            else if (Subscription* s = find_by_sub_id(ack->subs[i]))
            {
                s->granted_qos = rc;
                s->needs_resub = false;
            }
        }

        // Retire the tracking entry before the callback, not after. The
        // callback can end the session, and shutdown() clears pending_ -- after
        // which `ack` names an element of an empty vector and remove_pending
        // erases out of range. The entry has done its work by this point, and
        // a callback should not see a half-consumed table in any case.
        remove_pending(ack);

        on_suback_.call_if(v.packet_id, v.return_codes);
        return Error::Ok;
    }

    Error handle_unsuback(etl::span<const uint8_t> body) noexcept
    {
        const Result<uint16_t> id = codec::decode_packet_id(body);
        if (!id.ok())
            return id.error();

        PendingAck* ack = find_pending(id.value(), PacketType::Unsuback);
        if (ack == nullptr)
            return Error::Ok;

        for (const uint16_t sid : ack->subs)
            erase_subscription(sid);

        remove_pending(ack);
        return Error::Ok;
    }

    void dispatch(const Message& m) noexcept
    {
        bool handled = false;

        for (Subscription& s : subscriptions_)
        {
            if (!s.handler.is_valid())
                continue;
            if (topic_matches(etl::string_view(s.filter.data(), s.filter.size()), m.topic))
            {
                s.handler(m);
                handled = true;

                // A handler ended the session. The handlers after it would be
                // told about a message that arrived on a connection which no
                // longer exists, and every client call they made on the
                // strength of it would be refused.
                if (state_ != State::Connected)
                    return;
            }
        }

        if (!handled)
            on_message_.call_if(m);
    }

    Error send_ack(PacketType type, uint16_t id) noexcept
    {
        return enqueue_sized(packet_size(2), [&](etl::span<uint8_t> out) {
            return codec::encode_ack(out, type, id);
        });
    }

    //--------------------------------------------------------------------------
    // Timers
    //--------------------------------------------------------------------------

    Error pump_keep_alive() noexcept
    {
        if (keep_alive_ms_ == 0)
            return Error::Ok;   // keep-alive disabled

        const uint32_t now = clock_.now_ms();

        if (ping_outstanding_)
        {
            // The broker owes us a PINGRESP within one keep-alive period.
            if (elapsed_ms(now, ping_sent_ms_) > keep_alive_ms_)
                return Error::KeepAliveTimeout;
            return Error::Ok;
        }

        // Ping at 75% of the interval so a slow round trip still lands inside
        // the broker's 1.5x grace window.
        const uint32_t threshold = keep_alive_ms_ - (keep_alive_ms_ / 4u);
        if (elapsed_ms(now, last_sent_ms_) < threshold)
            return Error::Ok;

        const Error e = enqueue_sized(packet_size(0), [](etl::span<uint8_t> out) {
            return codec::encode_empty(out, PacketType::Pingreq);
        });
        if (e != Error::Ok)
            return Error::Ok;   // queue busy; try again next step()

        ping_outstanding_ = true;
        ping_sent_ms_     = now;
        return Error::Ok;
    }

    void pump_retransmit() noexcept
    {
        const bool forced = retransmit_now_;
        retransmit_now_   = false;

        if (Cfg::retry_interval_ms == 0 && !forced)
            return;

        const uint32_t now = clock_.now_ms();

        for (size_t i = 0; i < Cfg::max_inflight_out; ++i)
        {
            OutboundSlot& slot = outbound_[i];
            if (slot.phase == Phase::Free)
                continue;
            if (!forced && (Cfg::retry_interval_ms == 0 ||
                            elapsed_ms(now, slot.last_sent_ms) < Cfg::retry_interval_ms))
                continue;

            mark_dup(slot);
            if (enqueue(etl::span<const uint8_t>(slot.packet.data(), slot.packet_len)) ==
                Error::Ok)
                slot.last_sent_ms = now;
        }
    }

    /// Re-send subscriptions the broker has no record of, one batch per step so
    /// a large table cannot monopolise the transmit queue.
    void pump_resubscribe() noexcept
    {
        if (pending_.full())
            return;

        etl::vector<TopicSubscription, Cfg::max_topics_per_request> batch;
        etl::vector<uint16_t, Cfg::max_topics_per_request>          ids;

        for (const Subscription& s : subscriptions_)
        {
            if (!s.needs_resub)
                continue;

            batch.push_back(TopicSubscription{
                etl::string_view(s.filter.data(), s.filter.size()), s.requested_qos});
            ids.push_back(s.sub_id);

            if (batch.full())
                break;
        }

        if (batch.empty())
            return;

        const Result<uint32_t> rl = codec::subscribe_remaining_length(
            etl::span<const TopicSubscription>(batch.data(), batch.size()));
        if (!rl.ok())
            return;

        const uint16_t id = alloc_packet_id();
        if (id == 0)
            return;

        const Error e = enqueue_sized(packet_size(rl.value()), [&](etl::span<uint8_t> out) {
            return codec::encode_subscribe(
                out, id, etl::span<const TopicSubscription>(batch.data(), batch.size()));
        });
        if (e != Error::Ok)
            return;   // no room right now; unchanged flags mean we retry later

        PendingAck ack;
        ack.packet_id = id;
        ack.expected  = PacketType::Suback;
        ack.sent_ms   = clock_.now_ms();
        for (const uint16_t sid : ids)
        {
            ack.subs.push_back(sid);
            if (Subscription* s = find_by_sub_id(sid))
                s->needs_resub = false;
        }
        pending_.push_back(ack);
    }

    //--------------------------------------------------------------------------
    // Table helpers
    //--------------------------------------------------------------------------

    OutboundSlot* free_outbound_slot() noexcept
    {
        for (size_t i = 0; i < Cfg::max_inflight_out; ++i)
        {
            if (outbound_[i].phase == Phase::Free)
                return &outbound_[i];
        }
        return nullptr;
    }

    OutboundSlot* find_outbound(uint16_t id) noexcept
    {
        for (size_t i = 0; i < Cfg::max_inflight_out; ++i)
        {
            if (outbound_[i].phase != Phase::Free && outbound_[i].packet_id == id)
                return &outbound_[i];
        }
        return nullptr;
    }

    static void release_slot(OutboundSlot& slot) noexcept
    {
        slot.phase      = Phase::Free;
        slot.packet_id  = 0;
        slot.packet_len = 0;
    }

    /// Set the DUP flag on a stored PUBLISH. PUBREL has no DUP bit, so leave
    /// non-PUBLISH packets untouched.
    static void mark_dup(OutboundSlot& slot) noexcept
    {
        if (slot.packet_len == 0)
            return;
        const uint8_t type = static_cast<uint8_t>((slot.packet[0] >> 4) & 0x0F);
        if (type == static_cast<uint8_t>(PacketType::Publish))
            slot.packet[0] = static_cast<uint8_t>(slot.packet[0] | 0x08u);
    }

    bool inbound_contains(uint16_t id) const noexcept
    {
        for (const uint16_t v : inbound_ids_)
        {
            if (v == id)
                return true;
        }
        return false;
    }

    void inbound_remove(uint16_t id) noexcept
    {
        for (size_t i = 0; i < inbound_ids_.size(); ++i)
        {
            if (inbound_ids_[i] == id)
            {
                inbound_ids_.erase(inbound_ids_.begin() + i);
                return;
            }
        }
    }

    Subscription* find_subscription(etl::string_view filter) noexcept
    {
        for (Subscription& s : subscriptions_)
        {
            if (s.filter.size() == filter.size() &&
                std::memcmp(s.filter.data(), filter.data(), filter.size()) == 0)
                return &s;
        }
        return nullptr;
    }

    Subscription* find_by_sub_id(uint16_t sid) noexcept
    {
        for (Subscription& s : subscriptions_)
        {
            if (s.sub_id == sid)
                return &s;
        }
        return nullptr;
    }

    /// Drop the entry with this id, if it is still there. Safe to call for an
    /// id that has already gone: a stale ack simply finds nothing.
    void erase_subscription(uint16_t sid) noexcept
    {
        for (size_t i = 0; i < subscriptions_.size(); ++i)
        {
            if (subscriptions_[i].sub_id == sid)
            {
                subscriptions_.erase(subscriptions_.begin() + i);
                return;
            }
        }
    }

    /// Next free subscription id, skipping 0 and anything live. Wraps, so a
    /// long-lived client that churns subscriptions cannot collide with an
    /// entry that has outlasted a full trip round the counter.
    uint16_t alloc_sub_id() noexcept
    {
        for (uint32_t tries = 0; tries < 65535u; ++tries)
        {
            next_sub_id_ =
                (next_sub_id_ == 65535u) ? 1u : static_cast<uint16_t>(next_sub_id_ + 1u);
            if (find_by_sub_id(next_sub_id_) == nullptr)
                return next_sub_id_;
        }
        return 0;
    }

    PendingAck* find_pending(uint16_t id, PacketType expected) noexcept
    {
        for (PendingAck& a : pending_)
        {
            if (a.packet_id == id && a.expected == expected)
                return &a;
        }
        return nullptr;
    }

    void remove_pending(PendingAck* ack) noexcept
    {
        pending_.erase(pending_.begin() + (ack - pending_.data()));
    }

    bool packet_id_in_use(uint16_t id) const noexcept
    {
        for (size_t i = 0; i < Cfg::max_inflight_out; ++i)
        {
            if (outbound_[i].phase != Phase::Free && outbound_[i].packet_id == id)
                return true;
        }
        for (const PendingAck& a : pending_)
        {
            if (a.packet_id == id)
                return true;
        }
        return false;
    }

    /// Next free packet id, skipping 0 and anything currently outstanding.
    /// Returns 0 if every id is taken, which cannot happen with sane configs.
    uint16_t alloc_packet_id() noexcept
    {
        for (uint32_t tries = 0; tries < 65535u; ++tries)
        {
            next_packet_id_ =
                (next_packet_id_ == 65535u) ? 1u : static_cast<uint16_t>(next_packet_id_ + 1u);
            if (!packet_id_in_use(next_packet_id_))
                return next_packet_id_;
        }
        return 0;
    }

    void reset_session_state() noexcept
    {
        for (size_t i = 0; i < kOutSlots; ++i)
            release_slot(outbound_[i]);
        inbound_ids_.clear();
        pending_.clear();
        next_packet_id_ = 0;

        // The subscription table is deliberately *not* cleared. A clean session
        // discards the state held by the broker, not our own record of what the
        // application asked for -- and that record is the only thing that can
        // rebuild the session, since we do not keep the caller's strings alive.
        // Marking the table instead is what makes pump_resubscribe() fire after
        // the next CONNACK. Call unsubscribe() to actually forget a filter.
        for (Subscription& s : subscriptions_)
            s.needs_resub = true;
    }

    /// End the session, notify, and return the reason so callers can `return
    /// shutdown(e)` in one line.
    Error shutdown(Error reason) noexcept
    {
        const bool notify = (state_ != State::Idle);

        transport_.close();

        tx_.clear();
        rx_len_           = 0;
        ping_outstanding_ = false;
        state_            = State::Idle;
        last_error_       = reason;

        // Partially-sent packets are meaningless on a fresh connection. The
        // inflight table survives a non-clean session, because those messages
        // are still owed and get retransmitted after the next CONNECT; with a
        // clean session there is no session to resume, so it is discarded.
        pending_.clear();
        if (clean_session_)
        {
            for (size_t i = 0; i < kOutSlots; ++i)
                release_slot(outbound_[i]);
            inbound_ids_.clear();
        }

        for (Subscription& s : subscriptions_)
        {
            s.needs_resub = true;
        }

        if (notify)
            on_disconnect_.call_if(reason);

        return reason;
    }

    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------

    Transport& transport_;
    Clock&     clock_;

    TxQueue<Cfg::tx_buffer_size>             tx_{};
    etl::array<uint8_t, Cfg::rx_buffer_size> rx_{};
    size_t                                   rx_len_ = 0;

    etl::array<OutboundSlot, kOutSlots>  outbound_{};
    etl::vector<uint16_t, kInSlots>      inbound_ids_{};
    etl::vector<Subscription, kSubSlots> subscriptions_{};
    etl::vector<PendingAck, kAckSlots>   pending_{};

    // etl::delegate default-constructs to "unset"; no initialiser needed.
    MessageHandler    on_message_;
    ConnectHandler    on_connect_;
    DisconnectHandler on_disconnect_;
    DeliveryHandler   on_delivery_;
    SubackHandler     on_suback_;

    ConnackInfo connack_{};
    State       state_      = State::Idle;
    Error       last_error_ = Error::Ok;

    /// True while step() is on the stack, so a handler cannot re-enter it.
    bool in_step_ = false;

    uint32_t keep_alive_ms_          = 0;
    uint32_t last_sent_ms_           = 0;
    uint32_t last_received_ms_       = 0;
    uint32_t ping_sent_ms_           = 0;
    uint32_t connect_started_ms_     = 0;
    uint32_t inbound_overflow_count_ = 0;
    uint32_t tx_backpressure_count_  = 0;
    uint16_t next_packet_id_         = 0;
    uint16_t next_sub_id_            = 0;
    bool     ping_outstanding_       = false;
    bool     clean_session_          = true;
    bool     retransmit_now_         = false;
};

}   // namespace mqtt

#endif   // MQTT_CLIENT_HPP
