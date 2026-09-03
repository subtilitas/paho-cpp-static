// SPDX-License-Identifier: MIT
//
// An IPv4 address, parsed at compile time.
//
// Shared by the transports rather than duplicated in each: a second copy of a
// parser is a second place for it to be wrong. Nothing here touches a socket
// API, so it compiles on any platform -- which is the reason it is its own
// header, since tcp_ip_transport.hpp includes <sys/socket.h> and
// winsock_transport.hpp includes <winsock2.h>, and neither exists on the
// other's platform.

#ifndef MQTT_EXAMPLE_IPV4_HPP
#define MQTT_EXAMPLE_IPV4_HPP

#include <cstdint>

namespace example {

/// An IPv4 address as four octets, in the order they are written.
struct Ipv4
{
    uint8_t octets[4] = {};

    constexpr Ipv4() noexcept = default;
    constexpr Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept : octets{a, b, c, d} {}

    /// The address as a host-order 32-bit value, most significant octet first.
    constexpr uint32_t to_uint32() const noexcept
    {
        return (static_cast<uint32_t>(octets[0]) << 24) |
               (static_cast<uint32_t>(octets[1]) << 16) |
               (static_cast<uint32_t>(octets[2]) << 8) | static_cast<uint32_t>(octets[3]);
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

}   // namespace example

#endif   // MQTT_EXAMPLE_IPV4_HPP
