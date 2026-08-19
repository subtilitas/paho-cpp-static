// SPDX-License-Identifier: MIT
//
// The one thing you must implement to port this library.
//
// The core has no OS dependency: no sockets, no threads, no <chrono>, no
// <filesystem>. It talks to the network exclusively through this interface, so
// plain TCP, TLS (mbedTLS, wolfSSL, OpenSSL), lwIP, Zephyr's net stack and a
// unit-test double are all equally first-class.

#ifndef MQTT_TRANSPORT_HPP
#define MQTT_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>

#include <etl/span.h>

#include "mqtt/error.hpp"

namespace mqtt {

/// A non-blocking, ordered, reliable byte stream.
///
/// Implementations must never block. Every method returns promptly, reporting
/// Error::WouldBlock when it made no progress. The client is driven from
/// Client::step(), so a blocking implementation would stall the caller's whole
/// superloop.
///
/// TLS fits this interface without modification: do the TCP connect and the
/// handshake inside connect(), returning Error::WouldBlock until the handshake
/// completes. The MQTT layer neither knows nor cares whether the bytes are
/// encrypted. See examples/ for a worked mbedTLS-shaped adapter.
class Transport
{
public:
    Transport()          = default;
    virtual ~Transport() = default;

    /// Begin, or continue, establishing the connection.
    ///
    /// @retval Error::Ok               Connected and ready for send/recv.
    /// @retval Error::WouldBlock       Still in progress; call again later.
    /// @retval Error::TransportFailure Unrecoverable; the client will close.
    virtual Error connect() noexcept = 0;

    /// Write up to `data.size()` bytes. Partial writes are expected and normal.
    ///
    /// @param written Set to the number of bytes accepted. Must be set to 0 on
    ///                any non-Ok return.
    /// @retval Error::Ok               `written` bytes were accepted (may be 0).
    /// @retval Error::WouldBlock       No room right now; nothing was written.
    /// @retval Error::TransportClosed  Peer closed the connection.
    /// @retval Error::TransportFailure Unrecoverable error.
    virtual Error send(etl::span<const uint8_t> data, size_t& written) noexcept = 0;

    /// Read up to `buffer.size()` bytes.
    ///
    /// @param read Set to the number of bytes produced. Must be set to 0 on any
    ///             non-Ok return. Returning Ok with read == 0 is allowed and
    ///             treated as "nothing available".
    /// @retval Error::Ok               `read` bytes are in `buffer`.
    /// @retval Error::WouldBlock       Nothing available right now.
    /// @retval Error::TransportClosed  Peer performed an orderly shutdown.
    /// @retval Error::TransportFailure Unrecoverable error.
    virtual Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept = 0;

    /// Tear down the connection. Must be idempotent and must not fail.
    virtual void close() noexcept = 0;

    /// Whether the byte stream is currently usable.
    virtual bool is_connected() const noexcept = 0;

protected:
    // Protected rather than deleted: a derived transport may legitimately want
    // to be copyable, but assigning through a Transport& would slice it. This
    // is the shape the Core Guidelines recommend for an abstract interface
    // (C.67), and it keeps the class from silently acquiring the wrong ones.
    Transport(const Transport&)            = default;
    Transport& operator=(const Transport&) = default;
    Transport(Transport&&)                 = default;
    Transport& operator=(Transport&&)      = default;
};

/// A monotonic millisecond time source.
///
/// The value is free to wrap; the client only ever compares differences using
/// unsigned arithmetic, so a 32-bit counter rolling over every 49.7 days is
/// handled correctly and needs no special casing.
class Clock
{
public:
    Clock()          = default;
    virtual ~Clock() = default;

    /// Milliseconds since an arbitrary fixed origin. Must be monotonic.
    virtual uint32_t now_ms() const noexcept = 0;

protected:
    Clock(const Clock&)            = default;
    Clock& operator=(const Clock&) = default;
    Clock(Clock&&)                 = default;
    Clock& operator=(Clock&&)      = default;
};

/// Wrap-safe elapsed time. Correct across a uint32_t rollover.
constexpr uint32_t elapsed_ms(uint32_t now, uint32_t since) noexcept
{
    return now - since;  // unsigned wraparound is well defined and is the point
}

} // namespace mqtt

#endif // MQTT_TRANSPORT_HPP
