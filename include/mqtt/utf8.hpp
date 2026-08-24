// SPDX-License-Identifier: MIT
//
// UTF-8 validation for MQTT strings (MQTT 3.1.1 section 1.5.3).

#ifndef MQTT_UTF8_HPP
#define MQTT_UTF8_HPP

#include <etl/string_view.h>

namespace mqtt {

/// Is `s` a well-formed MQTT UTF-8 encoded string?
///
/// The spec's requirements, and what each one rules out:
///
///  - **MQTT-1.5.3-1** — the data must be well-formed UTF-8 as defined by
///    RFC 3629. That rejects truncated sequences, stray continuation bytes,
///    the obsolete five- and six-byte forms, code points above U+10FFFF, and
///    **overlong encodings** — a shorter value spelled in more bytes than it
///    needs. Overlongs are the interesting one: they let the same string be
///    written two ways, so a filter that inspects the bytes and a consumer
///    that decodes them can disagree about what they are looking at.
///  - **MQTT-1.5.3-1** also forbids encoding the surrogate halves
///    U+D800..U+DFFF, which are a UTF-16 mechanism and not characters.
///  - **MQTT-1.5.3-2** — the string must not contain U+0000. Legal UTF-8,
///    illegal here, and worth catching before it reaches a caller who passes
///    the pointer to something that stops at a NUL.
///
/// Deliberately *not* rejected:
///
///  - **U+FEFF**, the byte order mark. MQTT-1.5.3-3 says the data may contain
///    it and that a receiver must not skip or strip it, so it is an ordinary
///    character here.
///  - **Control characters** U+0001..U+001F and U+007F..U+009F. The spec says
///    a well-behaved implementation *should not* include them, not that it
///    must not, and rejecting on a SHOULD would drop traffic a conforming
///    peer is entitled to send.
///
/// O(n), no allocation, no recursion, no table. Safe from a constrained stack.
bool is_valid_mqtt_string(etl::string_view s) noexcept;

}   // namespace mqtt

#endif   // MQTT_UTF8_HPP
