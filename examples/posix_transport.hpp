// SPDX-License-Identifier: MIT
//
// A non-blocking BSD-socket Transport, for developing and smoke-testing on a
// host machine against a real broker.
//
// This file is deliberately OUTSIDE the library: the core has no OS dependency,
// and this is one possible adapter. Your target will have its own -- lwIP,
// Zephyr's net stack, FreeRTOS+TCP, an AT-command modem. The shape stays the
// same: connect() may report WouldBlock repeatedly, send() and recv() are
// partial and non-blocking, close() is idempotent.

#ifndef MQTT_POSIX_TRANSPORT_HPP
#define MQTT_POSIX_TRANSPORT_HPP

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mqtt/transport.hpp"
#include "posix_clock.hpp"   // example::PosixClock

namespace example {

class PosixTransport final : public mqtt::Transport
{
public:
    PosixTransport(const char* host, uint16_t port) noexcept : host_(host), port_(port) {}

    ~PosixTransport() override { close(); }

    mqtt::Error connect() noexcept override
    {
        if (fd_ >= 0 && connected_)
            return mqtt::Error::Ok;

        if (fd_ < 0)
        {
            char port_text[8];
            std::snprintf(port_text, sizeof(port_text), "%u", static_cast<unsigned>(port_));

            addrinfo hints{};
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            addrinfo* res = nullptr;
            if (::getaddrinfo(host_, port_text, &hints, &res) != 0 || res == nullptr)
                return mqtt::Error::TransportFailure;

            fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd_ < 0)
            {
                ::freeaddrinfo(res);
                return mqtt::Error::TransportFailure;
            }

            ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);

            int one = 1;
            ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            const int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
            ::freeaddrinfo(res);

            if (rc == 0)
            {
                connected_ = true;
                return mqtt::Error::Ok;
            }
            if (errno == EINPROGRESS || errno == EALREADY)
                return mqtt::Error::WouldBlock;

            close();
            return mqtt::Error::TransportFailure;
        }

        // Connection in progress: poll for completion via a zero-timeout select.
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd_, &write_set);
        timeval tv{0, 0};

        const int ready = ::select(fd_ + 1, nullptr, &write_set, nullptr, &tv);
        if (ready < 0)
        {
            close();
            return mqtt::Error::TransportFailure;
        }
        if (ready == 0)
            return mqtt::Error::WouldBlock;

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
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return mqtt::Error::WouldBlock;

        return (errno == EPIPE || errno == ECONNRESET) ? mqtt::Error::TransportClosed
                                                       : mqtt::Error::TransportFailure;
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
            return mqtt::Error::TransportClosed;   // orderly shutdown
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return mqtt::Error::WouldBlock;

        return (errno == ECONNRESET) ? mqtt::Error::TransportClosed
                                     : mqtt::Error::TransportFailure;
    }

    void close() noexcept override
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_        = -1;
        connected_ = false;
    }

    bool is_connected() const noexcept override { return connected_; }

    /// Block until the socket is readable or `timeout_ms` elapses. Optional --
    /// the client works fine with a busy loop, but on a host this keeps a demo
    /// from pinning a core.
    void wait_readable(int timeout_ms) const noexcept
    {
        if (fd_ < 0)
            return;

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd_, &read_set);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::select(fd_ + 1, &read_set, nullptr, nullptr, &tv);
    }

private:
    const char* host_      = nullptr;
    uint16_t    port_      = 0;
    int         fd_        = -1;
    bool        connected_ = false;
};

} // namespace example

#endif // MQTT_POSIX_TRANSPORT_HPP
