// SPDX-License-Identifier: MIT

#include "mqtt/topic.hpp"

#include "mqtt/utf8.hpp"

namespace mqtt {

bool is_valid_topic_name(etl::string_view name) noexcept
{
    if (name.empty() || name.size() > 65535u)
        return false;

    // MQTT-1.5.3-1. A topic name is an MQTT UTF-8 encoded string before it is
    // anything else, so a name that is not well-formed is not a name.
    if (!is_valid_mqtt_string(name))
        return false;

    for (const char c : name)
    {
        if (c == '+' || c == '#' || c == '\0')
            return false;
    }
    return true;
}

bool is_valid_filter(etl::string_view filter) noexcept
{
    if (filter.empty() || filter.size() > 65535u)
        return false;

    if (!is_valid_mqtt_string(filter))
        return false;

    size_t level_start = 0;

    for (size_t i = 0; i <= filter.size(); ++i)
    {
        const bool at_end = (i == filter.size());
        if (!at_end && filter[i] != '/')
        {
            if (filter[i] == '\0')
                return false;
            continue;
        }

        // We are at a level boundary: [level_start, i) is one level.
        const size_t level_len = i - level_start;

        if (level_len > 1)
        {
            // A wildcard must occupy an entire level, so a multi-character
            // level containing one is malformed ("sport+", "tennis#").
            for (size_t j = level_start; j < i; ++j)
            {
                if (filter[j] == '+' || filter[j] == '#')
                    return false;
            }
        }
        else if (level_len == 1 && filter[level_start] == '#')
        {
            // '#' is only legal as the very last level.
            if (!at_end)
                return false;
        }

        level_start = i + 1;
    }

    return true;
}

bool topic_matches(etl::string_view filter, etl::string_view topic) noexcept
{
    if (filter.empty() || topic.empty())
        return false;

    // A leading '$' topic is never matched by a filter whose first level is a
    // wildcard, so "#" does not accidentally subscribe to $SYS.
    if (topic[0] == '$')
    {
        if (filter[0] == '+' || filter[0] == '#')
            return false;
    }

    size_t f = 0;
    size_t t = 0;

    // Not `while (f < filter.size())`. A filter ending in '/' has an empty last
    // level, and so does the topic it should match: "a/" against "a/". With
    // that condition both indices advance past the final separator, the loop
    // exits before those two empty levels are compared, and the function falls
    // through to `return false` -- so such a filter matched nothing at all, not
    // even itself. MQTT 3.1.1 section 4.7.1 permits a zero-length level
    // anywhere, and both validators here accept these strings.
    //
    // Every exit is a return from inside: the loop ends when one side or the
    // other runs out, which the level comparison below detects.
    for (;;)
    {
        // Extract the next filter level.
        size_t f_end = f;
        while (f_end < filter.size() && filter[f_end] != '/')
            ++f_end;
        const etl::string_view f_level = filter.substr(f, f_end - f);

        if (f_level.size() == 1 && f_level[0] == '#')
        {
            // '#' matches this level and all levels beneath it. It also matches
            // the parent level, so "sport/#" matches "sport" -- but only when
            // the topic has been fully consumed at a level boundary.
            return true;
        }

        // Extract the corresponding topic level. `t` is only ever advanced to
        // t_end + 1 where t_end < topic.size(), so it cannot run past the end.
        size_t t_end = t;
        while (t_end < topic.size() && topic[t_end] != '/')
            ++t_end;
        const etl::string_view t_level = topic.substr(t, t_end - t);

        if (f_level.size() == 1 && f_level[0] == '+')
        {
            // '+' matches exactly one level, including an empty one.
        }
        else if (f_level != t_level)
        {
            return false;
        }

        // Advance both past the level and its separator.
        const bool f_more = (f_end < filter.size());
        const bool t_more = (t_end < topic.size());

        if (!f_more && !t_more)
            return true;   // both ran out together

        if (!f_more && t_more)
            return false;   // topic has extra levels the filter does not cover

        if (f_more && !t_more)
        {
            // The filter continues. The only way this still matches is a
            // trailing "/#", which covers the zero-or-more remaining levels.
            const etl::string_view rest = filter.substr(f_end + 1);
            return rest.size() == 1 && rest[0] == '#';
        }

        f = f_end + 1;
        t = t_end + 1;
    }
}

}   // namespace mqtt
