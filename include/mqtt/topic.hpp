// SPDX-License-Identifier: MIT
//
// Topic filter matching (MQTT 3.1.1 section 4.7).

#ifndef MQTT_TOPIC_HPP
#define MQTT_TOPIC_HPP

#include <etl/string_view.h>

namespace mqtt {

/// Does `topic` match subscription `filter`?
///
/// Implements the full wildcard rules: '+' matches exactly one level, '#'
/// matches the current level and everything below it and may only appear as the
/// final level. Topics beginning with '$' are not matched by a filter that
/// starts with a wildcard, as the spec requires.
///
/// Runs in O(len) with no allocation and no recursion, so it is safe to call
/// from a constrained stack.
bool topic_matches(etl::string_view filter, etl::string_view topic) noexcept;

/// Is `filter` a well-formed subscription filter?
/// Rejects '#' in a non-final position, and '+'/'#' that do not occupy a whole
/// level (for example "sport+" or "sport/tennis#").
bool is_valid_filter(etl::string_view filter) noexcept;

/// Is `name` a well-formed topic *name* for publishing?
/// Topic names must be non-empty and must contain no wildcard characters.
bool is_valid_topic_name(etl::string_view name) noexcept;

} // namespace mqtt

#endif // MQTT_TOPIC_HPP
