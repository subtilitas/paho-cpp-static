// SPDX-License-Identifier: MIT
//
// A plain TCP transport to a numeric IPv4 address, on Winsock. No DNS.
//
// The Windows counterpart of tcp_ip_transport.hpp, and deliberately the same
// shape: fill a sockaddr_in, connect non-blocking, poll for completion. The
// library itself has no OS dependency, so the whole Windows story is this file
// and win_clock.hpp -- which is the claim this example exists to demonstrate.
//
// Four things differ from the POSIX version, and they are the four things that
// catch every port:
//
//   1. Winsock needs WSAStartup() before any socket call and WSACleanup()
//      after the last one. The count is per process, so this class keeps a
//      reference count rather than assuming it is the only user.
//   2. A socket is a SOCKET, not an int, and the invalid value is
//      INVALID_SOCKET rather than -1. SOCKET is unsigned, so `fd < 0` -- the
//      POSIX idiom -- is always false and silently wrong.
//   3. Errors come from WSAGetLastError(), not errno, and use WSAE* names.
//   4. send() and recv() take `char*` and an `int` length, so a buffer larger
//      than INT_MAX has to be clamped rather than truncated by conversion.
//
// Link ws2_32.
//
// Include order matters on Windows: windows.h pulls in winsock 1, which
// conflicts with winsock2.h. Both this header and win_clock.hpp define
// WIN32_LEAN_AND_MEAN first, which suppresses that, so the two can be included
// in either order.

#ifndef MQTT_WINSOCK_TRANSPORT_HPP
#define MQTT_WINSOCK_TRANSPORT_HPP

#if !defined(_WIN32)
#error "winsock_transport.hpp is for Windows; use tcp_ip_transport.hpp elsewhere"
#endif

// Keep <windows.h> from pulling in winsock 1, which conflicts with winsock2.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
// ws2tcpip.h must follow winsock2.h.
#include <ws2tcpip.h>

#include <climits>
#include <cstdint>

#include "ipv4.hpp"
#include "mqtt/transport.hpp"

namespace example {

class WinsockTransport final : public mqtt::Transport
{
public:
    WinsockTransport(Ipv4 address, uint16_t port) noexcept : address_(address), port_(port) {}

    /// Convenience constructor for a dotted-quad string.
    WinsockTransport(const char* dotted_quad, uint16_t port) noexcept
        : address_(ipv4(dotted_quad)), port_(port)
    {
    }

    ~WinsockTransport() override { close(); }

    WinsockTransport(const WinsockTransport&)            = delete;
    WinsockTransport& operator=(const WinsockTransport&) = delete;

    mqtt::Error connect() noexcept override
    {
        if (connected_)
            return mqtt::Error::Ok;

        if (sock_ == INVALID_SOCKET)
            return begin_connect();

        return finish_connect();
    }

    mqtt::Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = 0;
        if (sock_ == INVALID_SOCKET || !connected_)
            return mqtt::Error::TransportClosed;

        const int len = clamp_to_int(data.size());
        const int n   = ::send(sock_, reinterpret_cast<const char*>(data.data()), len, 0);

        if (n > 0)
        {
            written = static_cast<size_t>(n);
            return mqtt::Error::Ok;
        }
        if (n == 0)
            return mqtt::Error::TransportClosed;

        return classify(::WSAGetLastError());
    }

    mqtt::Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (sock_ == INVALID_SOCKET || !connected_)
            return mqtt::Error::TransportClosed;

        const int len = clamp_to_int(buffer.size());
        const int n   = ::recv(sock_, reinterpret_cast<char*>(buffer.data()), len, 0);

        if (n > 0)
        {
            read = static_cast<size_t>(n);
            return mqtt::Error::Ok;
        }

        // 0 from recv() is an orderly shutdown by the peer, not "no data".
        if (n == 0)
            return mqtt::Error::TransportClosed;

        return classify(::WSAGetLastError());
    }

    void close() noexcept override
    {
        if (sock_ != INVALID_SOCKET)
        {
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        connected_ = false;
        stop_winsock();
    }

    bool is_connected() const noexcept override { return connected_; }

    /// Block until the socket has something to read, or the timeout expires.
    ///
    /// Not part of the Transport interface, and nothing in the library calls
    /// it -- the client never blocks. It exists so a demo loop can idle rather
    /// than spin. A real application waits on whatever its event loop waits on.
    void wait_readable(int timeout_ms) const noexcept
    {
        if (sock_ == INVALID_SOCKET)
            return;

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock_, &read_set);

        timeval tv{};
        tv.tv_sec  = static_cast<long>(timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

        // The first argument is ignored on Winsock; the set carries the socket.
        (void)::select(0, &read_set, nullptr, nullptr, &tv);
    }

private:
    /// WSAStartup is per process and reference counted by Winsock itself, but
    /// only if every startup is matched by a cleanup. Tracking it here keeps a
    /// second transport in the same process working after the first closes.
    bool start_winsock() noexcept
    {
        if (wsa_started_)
            return true;

        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;

        wsa_started_ = true;
        return true;
    }

    void stop_winsock() noexcept
    {
        if (wsa_started_)
        {
            ::WSACleanup();
            wsa_started_ = false;
        }
    }

    mqtt::Error begin_connect() noexcept
    {
        if (!start_winsock())
            return mqtt::Error::TransportFailure;

        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET)
        {
            stop_winsock();
            return mqtt::Error::TransportFailure;
        }

        u_long non_blocking = 1;
        if (::ioctlsocket(sock_, FIONBIO, &non_blocking) != 0)
        {
            close();
            return mqtt::Error::TransportFailure;
        }

        // Nagle delays a small PUBLISH behind an unacknowledged one, which on a
        // request/response protocol shows up as latency nobody asked for.
        // int rather than BOOL: BOOL comes from windows.h, and winsock2.h does
        // not promise to have pulled it in. setsockopt takes the bytes either
        // way, so the narrower dependency is free.
        int no_delay = 1;
        (void)::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&no_delay),
                           static_cast<int>(sizeof no_delay));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = ::htons(port_);
        addr.sin_addr.s_addr = ::htonl(address_.to_uint32());

        // Winsock's connect takes an int length, not a socklen_t.
        if (::connect(sock_, reinterpret_cast<const sockaddr*>(&addr),
                      static_cast<int>(sizeof addr)) == 0)
        {
            connected_ = true;
            return mqtt::Error::Ok;
        }

        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY)
            return mqtt::Error::WouldBlock;

        close();
        return mqtt::Error::TransportFailure;
    }

    /// A non-blocking connect reports completion through writability. Success
    /// and failure both make the socket writable, so SO_ERROR decides which.
    mqtt::Error finish_connect() noexcept
    {
        fd_set write_set;
        fd_set except_set;
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        FD_SET(sock_, &write_set);
        FD_SET(sock_, &except_set);

        timeval immediately{};   // poll, never block: the client must not stall

        const int ready = ::select(0, nullptr, &write_set, &except_set, &immediately);
        if (ready == 0)
            return mqtt::Error::WouldBlock;

        if (ready == SOCKET_ERROR)
        {
            close();
            return mqtt::Error::TransportFailure;
        }

        int       so_error = 0;
        int       len      = static_cast<int>(sizeof so_error);
        const int rc =
            ::getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);

        if (rc != 0 || so_error != 0)
        {
            close();
            return mqtt::Error::TransportFailure;
        }

        connected_ = true;
        return mqtt::Error::Ok;
    }

    /// send() and recv() take an int. A span longer than INT_MAX is handed over
    /// in pieces rather than wrapped into a negative length.
    static int clamp_to_int(size_t n) noexcept
    {
        constexpr size_t kMax = static_cast<size_t>(INT_MAX);
        return static_cast<int>(n < kMax ? n : kMax);
    }

    mqtt::Error classify(int err) noexcept
    {
        if (err == WSAEWOULDBLOCK || err == WSAEINTR)
            return mqtt::Error::WouldBlock;

        if (err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAENETRESET ||
            err == WSAESHUTDOWN || err == WSAENOTCONN)
        {
            close();
            return mqtt::Error::TransportClosed;
        }

        close();
        return mqtt::Error::TransportFailure;
    }

    Ipv4     address_{};
    uint16_t port_ = 0;

    SOCKET sock_        = INVALID_SOCKET;
    bool   connected_   = false;
    bool   wsa_started_ = false;
};

}   // namespace example

#endif   // MQTT_WINSOCK_TRANSPORT_HPP
