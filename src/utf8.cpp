// SPDX-License-Identifier: MIT

#include "mqtt/utf8.hpp"

#include <cstdint>

namespace mqtt {
namespace {

/// A string_view yields char, whose signedness is implementation-defined. Every
/// comparison below is on the bit pattern, so convert once, here, rather than
/// leaving a signed char to sign-extend into a comparison somewhere later.
constexpr uint8_t byte_at(etl::string_view s, size_t i) noexcept
{
    return static_cast<uint8_t>(s[i]);
}

/// Smallest code point each sequence length is allowed to encode. A value below
/// its entry is an overlong: legal to decode, illegal to have written.
constexpr uint32_t kSmallestFor[4] = {0x00000000u, 0x00000080u, 0x00000800u, 0x00010000u};

constexpr uint32_t kMaxCodePoint   = 0x0010FFFFu;
constexpr uint32_t kSurrogateFirst = 0x0000D800u;
constexpr uint32_t kSurrogateLast  = 0x0000DFFFu;

}   // namespace

bool is_valid_mqtt_string(etl::string_view s) noexcept
{
    const size_t n = s.size();
    size_t       i = 0;

    while (i < n)
    {
        const uint8_t lead = byte_at(s, i);

        // MQTT-1.5.3-2. Checked first because it is a rule about MQTT rather
        // than about UTF-8: U+0000 is perfectly well-formed and still banned.
        if (lead == 0x00u)
            return false;

        size_t   continuations = 0;
        uint32_t code_point    = 0;

        if (lead < 0x80u)
        {
            continuations = 0;
            code_point    = lead;
        }
        else if ((lead & 0xE0u) == 0xC0u)
        {
            continuations = 1;
            code_point    = static_cast<uint32_t>(lead & 0x1Fu);
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            continuations = 2;
            code_point    = static_cast<uint32_t>(lead & 0x0Fu);
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            continuations = 3;
            code_point    = static_cast<uint32_t>(lead & 0x07u);
        }
        else
        {
            // 0x80..0xBF is a continuation byte with nothing to continue;
            // 0xF8..0xFF would begin the five- and six-byte forms that
            // RFC 3629 removed when it capped Unicode at U+10FFFF.
            return false;
        }

        // Written as a subtraction so it cannot overflow on a 16-bit size_t,
        // which the equivalent `i + continuations >= n` can.
        if (n - i <= continuations)
            return false;   // sequence runs off the end of the string

        for (size_t k = 1; k <= continuations; ++k)
        {
            const uint8_t cont = byte_at(s, i + k);
            if ((cont & 0xC0u) != 0x80u)
                return false;   // not a continuation byte
            code_point = (code_point << 6) | static_cast<uint32_t>(cont & 0x3Fu);
        }

        // The shortest encoding is the only legal one -- the same rule
        // vbi_decode applies to variable byte integers, and for the same
        // reason: two spellings of one value is an ambiguity someone gets to
        // choose between.
        if (code_point < kSmallestFor[continuations])
            return false;

        if (code_point >= kSurrogateFirst && code_point <= kSurrogateLast)
            return false;

        if (code_point > kMaxCodePoint)
            return false;

        i += continuations + 1u;
    }

    return true;
}

}   // namespace mqtt
