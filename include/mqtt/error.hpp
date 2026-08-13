// SPDX-License-Identifier: MIT
//
// Error codes and a lightweight expected-like Result type.
// No exceptions are used or required anywhere in this library.

#ifndef MQTT_ERROR_HPP
#define MQTT_ERROR_HPP

#include <cstdint>

#include <etl/utility.h>

//------------------------------------------------------------------------------
// Build policy assertions
//
// The build system asks for exceptions and RTTI to be switched off, but the
// flags that do it differ per compiler and are easy to get wrong -- MSVC needs
// /EHsc removed rather than a flag added, and a typo there fails silently,
// leaving a library that quietly contradicts its own documentation.
//
// So the request is passed down as MQTT_REQUIRE_NO_* and checked here against
// what the compiler actually did. A mismatch is a build error, not a surprise
// discovered later in a linker map.
//------------------------------------------------------------------------------

#if defined(MQTT_REQUIRE_NO_EXCEPTIONS)
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#error "Exceptions are enabled, but this build asked for them to be off. \
On MSVC that usually means /EHsc survived in CMAKE_CXX_FLAGS. \
Set MQTT_NO_EXCEPTIONS=OFF if you intend to build with exceptions."
#endif
#endif

#if defined(MQTT_REQUIRE_NO_RTTI)
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
#error "RTTI is enabled, but this build asked for it to be off. \
Set MQTT_NO_RTTI=OFF if you intend to build with RTTI."
#endif
#endif

namespace mqtt {

/// Every fallible operation in this library reports failure through this enum.
/// There are no exceptions and no error globals.
enum class Error : int16_t
{
    Ok = 0,

    // --- flow control (not really failures) ---
    WouldBlock,        ///< Operation incomplete; call again later. Never an error.
    Incomplete,        ///< A partial packet was consumed; more bytes needed.

    // --- caller / configuration errors ---
    InvalidArgument,   ///< A parameter failed validation.
    NotConnected,      ///< Operation requires an established MQTT session.
    AlreadyConnected,  ///< connect() called while a session is active.
    NotSupported,      ///< Feature intentionally omitted from this build.

    // --- capacity exhaustion (the interesting ones on a static build) ---
    BufferTooSmall,    ///< Serialization target ran out of room.
    TxQueueFull,       ///< Outgoing byte queue cannot accept the packet right now.
    NoInflightSlot,    ///< All QoS>0 outbound slots are occupied.
    NoInboundSlot,     ///< All QoS 2 inbound tracking slots are occupied.
    NoSubscriptionSlot,///< Subscription table is full.
    NoPendingAckSlot,  ///< SUBSCRIBE/UNSUBSCRIBE ack tracking table is full.
    PayloadTooLarge,   ///< Message exceeds the configured persisted-message size.
    TopicTooLong,      ///< Topic or filter exceeds Config::max_topic_len.

    // --- protocol / peer errors ---
    MalformedPacket,   ///< Bytes on the wire did not parse.
    ProtocolViolation, ///< Peer did something the spec forbids.
    PacketTooLarge,    ///< Inbound packet exceeds the receive buffer.
    ConnectionRefused, ///< Broker rejected CONNECT; see Client::connack().
    KeepAliveTimeout,  ///< No PINGRESP within the keep-alive window.
    ConnectTimeout,    ///< Handshake did not finish within connect_timeout_ms.

    // --- transport errors ---
    TransportFailure,  ///< Underlying transport reported an unrecoverable error.
    TransportClosed,   ///< Peer closed the connection.
};

/// Human-readable name for an error, for logging. Returns a static string;
/// never allocates.
const char* to_string(Error e) noexcept;

/// True for codes that mean "nothing went wrong, just try again".
constexpr bool is_retryable(Error e) noexcept
{
    return e == Error::WouldBlock || e == Error::Incomplete;
}

//------------------------------------------------------------------------------
// Result<T>
//------------------------------------------------------------------------------

/// A minimal expected-like carrier. Trivially copyable for trivial T, holds no
/// dynamic storage, and never throws. Reading value() when !ok() is a
/// programming error and returns the default-constructed value rather than
/// terminating, so a mis-checked call degrades predictably.
template <typename T>
class Result
{
public:
    constexpr Result(T value) noexcept : value_(value), error_(Error::Ok) {}
    constexpr Result(Error e) noexcept : value_(T{}), error_(e) {}

    constexpr bool ok() const noexcept { return error_ == Error::Ok; }
    constexpr explicit operator bool() const noexcept { return ok(); }

    constexpr Error error() const noexcept { return error_; }
    constexpr T value() const noexcept { return ok() ? value_ : T{}; }
    constexpr T value_or(T fallback) const noexcept { return ok() ? value_ : fallback; }

private:
    T     value_;
    Error error_;
};

/// Propagate an error out of the current function if `expr` failed.
/// Used heavily in the codec so every bounds check is visible but not noisy.
#define MQTT_TRY(expr)                                    \
    do                                                    \
    {                                                     \
        const ::mqtt::Error mqtt_try_rc_ = (expr);        \
        if (mqtt_try_rc_ != ::mqtt::Error::Ok)            \
            return mqtt_try_rc_;                          \
    } while (false)

/// Convert a bool-returning ETL stream write into an Error.
#define MQTT_WRITE(expr) \
    do { if (!(expr)) return ::mqtt::Error::BufferTooSmall; } while (false)

/// Convert an etl::optional-returning ETL stream read into an Error.
#define MQTT_READ(dest, expr)                             \
    do                                                    \
    {                                                     \
        auto mqtt_opt_ = (expr);                          \
        if (!mqtt_opt_.has_value())                       \
            return ::mqtt::Error::MalformedPacket;        \
        (dest) = mqtt_opt_.value();                       \
    } while (false)

} // namespace mqtt

#endif // MQTT_ERROR_HPP
