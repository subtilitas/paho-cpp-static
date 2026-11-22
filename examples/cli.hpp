// SPDX-License-Identifier: MIT
//
// Command-line helpers shared by the host examples: strict port parsing and
// writing a length-delimited view to a std::ostream.
//
// Host-only, like the rest of examples/. The library itself does no I/O.

#ifndef MQTT_EXAMPLE_CLI_HPP
#define MQTT_EXAMPLE_CLI_HPP

#include <cstdint>
#include <ostream>

#include <etl/string_view.h>
#include <etl/to_arithmetic.h>

namespace example {

/// Lowest and highest TCP port a client can connect to.
constexpr uint32_t kPortMin = 1u;
constexpr uint32_t kPortMax = 65535u;

/// Parse a TCP port from `text`: decimal digits only, 1 to 65535.
///
/// Returns false and leaves `port` unchanged for anything else: an empty
/// string, a sign, a non-digit, a value that overflows uint32_t, 0, or a value
/// above 65535. "70000" is rejected rather than wrapped to 4464.
inline bool parse_port(const char* text, uint16_t& port) noexcept
{
    if (text == nullptr)
        return false;

    const etl::string_view digits(text);
    if (digits.empty() || (digits.front() < '0') || (digits.front() > '9'))
        return false;

    const etl::to_arithmetic_result<uint32_t> parsed = etl::to_arithmetic<uint32_t>(digits);
    if (!parsed.has_value())
        return false;

    const uint32_t value = parsed.value();
    if ((value < kPortMin) || (value > kPortMax))
        return false;

    port = static_cast<uint16_t>(value);
    return true;
}

/// A view written to a stream by length, not up to a terminator. Topics and
/// payloads in mqtt::Message are views into the receive buffer and are not
/// null-terminated.
struct Text
{
    etl::string_view view;
};

inline std::ostream& operator<<(std::ostream& os, Text t)
{
    return os.write(t.view.data(), static_cast<std::streamsize>(t.view.size()));
}

}   // namespace example

#endif   // MQTT_EXAMPLE_CLI_HPP
