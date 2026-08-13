// SPDX-License-Identifier: MIT
//
// A plain TCP transport to a numeric IPv4 address. No DNS.
//
// This is the shape you actually ship. posix_transport.hpp calls getaddrinfo(),
// which is convenient on a workstation but is often absent on an embedded
// stack, drags in a resolver, and allocates. When the broker lives at a fixed
// address -- a gateway on the local network, a static IP from DHCP reservation,
// an address you already resolved once and cached -- none of that is needed.
//
// What is left is the minimum: fill a sockaddr_in, connect non-blocking, poll
// for completion. Roughly forty lines shorter than the getaddrinfo version and
// with no hidden allocation anywhere.
//
// Porting this to lwIP is close to mechanical: sockaddr_in becomes ip4_addr_t,
// ::socket/::connect become lwip_socket/lwip_connect (or the netconn
// equivalents), and the poll() call becomes lwip_poll or a netconn callback.

#ifndef MQTT_TCP_IP_TRANSPORT_HPP
#define MQTT_TCP_IP_TRANSPORT_HPP

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mqtt/transport.hpp"

namespace example {

//------------------------------------------------------------------------------
// Address
//------------------------------------------------------------------------------

/// An IPv4 address as four octets, in the order they are written.
struct Ipv4
{
    uint8_t octets[4] = {};

    constexpr Ipv4() noexcept = default;
    constexpr Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept
        : octets{a, b, c, d}
    {
    }

    /// The address as a host-order 32-bit value, most significant octet first.
    constexpr uint32_t to_uint32() const noexcept
    {
        return (static_cast<uint32_t>(octets[0]) << 24) |
               (static_cast<uint32_t>(octets[1]) << 16) |
               (static_cast<uint32_t>(octets[2]) << 8) |
               static_cast<uint32_t>(octets[3]);
    }
};

/// Parse a dotted-quad such as "192.168.1.50".
///
/// Hand-rolled rather than inet_pton() so it is constexpr, allocation-free and
/// available on targets with no resolver at all. Strict: rejects out-of-range
/// octets, empty octets, more or fewer than four of them, and any character
/// that is not a digit or a dot.
///
/// Because it is constexpr you can validate a compiled-in address at build
/// time:
/// @code
/// constexpr example::Ipv4 kBroker = example::ipv4("192.168.1.50");
/// @endcode
constexpr bool parse_ipv4(const char* text, Ipv4& out) noexcept
{
    if (text == nullptr)
        return false;

    Ipv4     result{};
    uint32_t value  = 0;
    int      digits = 0;
    int      octet  = 0;

    for (const char* p = text;; ++p)
    {
        const char c = *p;

        if (c >= '0' && c <= '9')
        {
            if (digits == 3)
                return false;
            value = (value * 10u) + static_cast<uint32_t>(c - '0');
            if (value > 255u)
                return false;
            ++digits;
        }
        else if (c == '.' || c == '\0')
        {
            if (digits == 0 || octet >= 4)
                return false;
            result.octets[octet] = static_cast<uint8_t>(value);
            ++octet;
            value  = 0;
            digits = 0;
            if (c == '\0')
                break;
        }
        else
        {
            return false;
        }
    }

    if (octet != 4)
        return false;

    out = result;
    return true;
}

/// Convenience form for literals. An unparseable address yields 0.0.0.0, which
/// will fail to connect rather than silently reaching somewhere unexpected.
constexpr Ipv4 ipv4(const char* text) noexcept
{
    Ipv4 result{};
    return parse_ipv4(text, result) ? result : Ipv4{};
}

//------------------------------------------------------------------------------
// Transport
//------------------------------------------------------------------------------

class TcpIpTransport final : public mqtt::Transport
{
public:
    TcpIpTransport(Ipv4 address, uint16_t port) noexcept : address_(address), port_(port) {}

    /// Convenience constructor for a dotted-quad string.
    TcpIpTransport(const char* dotted_quad, uint16_t port) noexcept
        : address_(ipv4(dotted_quad)), port_(port)
    {
    }

    ~TcpIpTransport() override { close(); }

    TcpIpTransport(const TcpIpTransport&)            = delete;
    TcpIpTransport& operator=(const TcpIpTransport&) = delete;

    mqtt::Error connect() noexcept override
    {
        if (connected_)
            return mqtt::Error::Ok;

        if (fd_ < 0)
            return begin_connect();

        return finish_connect();
    }

    mqtt::Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = 0;
        if (fd_ < 0 || !connected_)
            return mqtt::Error::TransportClosed;

        const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n > 0)
        {
            written = static_cast<size_t>(n);
            return mqtt::Error::Ok;
        }
        if (n == 0)
            return mqtt::Error::TransportClosed;

        return classify(errno);
    }

    mqtt::Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (fd_ < 0 || !connected_)
            return mqtt::Error::TransportClosed;

        const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (n > 0)
        {
            read = static_cast<size_t>(n);
            return mqtt::Error::Ok;
        }
        if (n == 0)
            return mqtt::Error::TransportClosed;   // orderly shutdown by the peer

        return classify(errno);
    }

    void close() noexcept override
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_        = -1;
        connected_ = false;
    }

    bool is_connected() const noexcept override { return connected_; }

    /// Sleep until the socket is readable or `timeout_ms` elapses.
    ///
    /// Entirely optional -- the client is happy being polled in a busy loop.
    /// On a host it keeps a demo from pinning a core; on a target you would
    /// more likely block on a queue or a task notification instead.
    void wait_readable(int timeout_ms) const noexcept
    {
        if (fd_ < 0)
            return;

        pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;
        ::poll(&pfd, 1, timeout_ms);
    }

private:
    mqtt::Error begin_connect() noexcept
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0)
            return mqtt::Error::TransportFailure;

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            close();
            return mqtt::Error::TransportFailure;
        }

        // MQTT packets are small and latency-sensitive; Nagle would sit on an
        // acknowledgement waiting for more data that is not coming.
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // No getaddrinfo, no resolver, no allocation: the address is built by
        // hand from the four octets.
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port_);
        addr.sin_addr.s_addr = htonl(address_.to_uint32());

        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            connected_ = true;
            return mqtt::Error::Ok;   // loopback and LAN peers often land here
        }

        if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR)
            return mqtt::Error::WouldBlock;

        close();
        return mqtt::Error::TransportFailure;
    }

    mqtt::Error finish_connect() noexcept
    {
        // Zero timeout: ask whether the handshake finished, never wait for it.
        pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLOUT;

        const int ready = ::poll(&pfd, 1, 0);
        if (ready < 0)
        {
            if (errno == EINTR)
                return mqtt::Error::WouldBlock;
            close();
            return mqtt::Error::TransportFailure;
        }
        if (ready == 0)
            return mqtt::Error::WouldBlock;

        // Writable does not mean successful -- a refused connection is also
        // reported as writable, so the pending error has to be collected.
        int       err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        {
            close();
            return mqtt::Error::TransportFailure;
        }

        connected_ = true;
        return mqtt::Error::Ok;
    }

    static mqtt::Error classify(int e) noexcept
    {
        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
            return mqtt::Error::WouldBlock;
        if (e == EPIPE || e == ECONNRESET || e == ENOTCONN)
            return mqtt::Error::TransportClosed;
        return mqtt::Error::TransportFailure;
    }

    Ipv4     address_{};
    uint16_t port_      = 0;
    int      fd_        = -1;
    bool     connected_ = false;
};

} // namespace example

#endif // MQTT_TCP_IP_TRANSPORT_HPP
