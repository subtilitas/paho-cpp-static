#include "test_harness.hpp"

#include <cstdio>

#include <etl/string.h>
#include <etl/vector.h>

#include "mqtt/topic.hpp"

using namespace mqtt;

namespace {

bool matches(const char* filter, const char* topic) noexcept
{
    return topic_matches(etl::string_view(filter), etl::string_view(topic));
}

}   // namespace

TEST(topic_exact_matches)
{
    CHECK(matches("sport/tennis/player1", "sport/tennis/player1"));
    CHECK(!matches("sport/tennis/player1", "sport/tennis/player2"));
    CHECK(!matches("sport/tennis", "sport/tennis/player1"));
    CHECK(!matches("sport/tennis/player1", "sport/tennis"));
}

TEST(topic_multilevel_wildcard)
{
    // Examples straight out of MQTT 3.1.1 section 4.7.1.2.
    CHECK(matches("sport/tennis/player1/#", "sport/tennis/player1"));
    CHECK(matches("sport/tennis/player1/#", "sport/tennis/player1/ranking"));
    CHECK(matches("sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon"));

    CHECK(matches("sport/#", "sport"));
    CHECK(matches("sport/#", "sport/tennis"));
    CHECK(matches("#", "anything/at/all"));
    CHECK(matches("#", "a"));

    CHECK(!matches("sport/tennis/#", "sports/tennis"));
}

TEST(topic_singlelevel_wildcard)
{
    // Section 4.7.1.3.
    CHECK(matches("sport/tennis/+", "sport/tennis/player1"));
    CHECK(matches("sport/tennis/+", "sport/tennis/player2"));
    CHECK(!matches("sport/tennis/+", "sport/tennis/player1/ranking"));

    CHECK(!matches("sport/+", "sport"));
    CHECK(matches("sport/+", "sport/"));

    CHECK(matches("+/+", "/finance"));
    CHECK(matches("/+", "/finance"));
    CHECK(!matches("+", "/finance"));
}

TEST(topic_dollar_topics_are_shielded_from_wildcards)
{
    // Section 4.7.2: a leading wildcard must not reach $SYS.
    CHECK(!matches("#", "$SYS/broker/uptime"));
    CHECK(!matches("+/monitor/clients", "$SYS/monitor/clients"));
    CHECK(matches("$SYS/#", "$SYS/broker/uptime"));
    CHECK(matches("$SYS/monitor/+", "$SYS/monitor/clients"));
}

TEST(topic_filter_validation)
{
    CHECK(is_valid_filter("sport/tennis/#"));
    CHECK(is_valid_filter("#"));
    CHECK(is_valid_filter("+"));
    CHECK(is_valid_filter("+/tennis/#"));
    CHECK(is_valid_filter("sport/+/player1"));

    CHECK(!is_valid_filter(""));
    CHECK(!is_valid_filter("sport/tennis#"));         // '#' not its own level
    CHECK(!is_valid_filter("sport/tennis/#/rank"));   // '#' not final
    CHECK(!is_valid_filter("sport+"));                // '+' not its own level
}

TEST(topic_name_validation)
{
    CHECK(is_valid_topic_name("sport/tennis/player1"));
    CHECK(is_valid_topic_name("a"));
    CHECK(is_valid_topic_name("/"));

    CHECK(!is_valid_topic_name(""));
    CHECK(!is_valid_topic_name("sport/+"));   // wildcards are illegal in a name
    CHECK(!is_valid_topic_name("sport/#"));
}

TEST(topic_empty_levels_are_significant)
{
    CHECK(matches("a//b", "a//b"));
    CHECK(!matches("a/b", "a//b"));
    CHECK(matches("a/+/b", "a//b"));
}

TEST(a_filter_ending_in_a_separator_matches_its_own_topic)
{
    // The trailing separator makes an empty last level, and MQTT 3.1.1 section
    // 4.7.1 permits a zero-length level anywhere. Both validators accept these
    // strings and a broker forwards them, so a filter that matched nothing --
    // not even the topic that spells it exactly -- dropped every message for
    // the subscription silently, with no error code and no log line.
    CHECK(matches("a/", "a/"));
    CHECK(matches("sport/", "sport/"));
    CHECK(matches("/", "/"));
    CHECK(matches("a//", "a//"));
    CHECK(matches("a/+/", "a/b/"));
    CHECK(matches("a/#", "a/"));

    // The level is empty, not absent: "a/" and "a" are different topics.
    CHECK(!matches("a/", "a"));
    CHECK(!matches("a", "a/"));
    CHECK(!matches("a/", "a/b"));
    CHECK(!matches("a/+/", "a/b/c"));
}

namespace {

/// A second implementation of MQTT 3.1.1 section 4.7, written for obviousness
/// rather than speed, to compare the real one against.
///
/// The point of a second implementation is that it fails differently. A filter
/// that never matches anything is not a crash and not an error code, so no
/// sanitizer, coverage gate or fuzzer without an oracle can see it -- the
/// function returns cleanly with the wrong answer. Only a differential check
/// does. This one is what found the trailing-separator defect above.
bool reference_matches(etl::string_view filter, etl::string_view topic) noexcept
{
    if (filter.empty() || topic.empty())
        return false;

    if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#'))
        return false;

    size_t f = 0;
    size_t t = 0;

    for (;;)
    {
        // One level from each side.
        size_t fe = f;
        while (fe < filter.size() && filter[fe] != '/')
            ++fe;
        size_t te = t;
        while (te < topic.size() && topic[te] != '/')
            ++te;

        const etl::string_view fl = filter.substr(f, fe - f);

        if (fl.size() == 1 && fl[0] == '#')
            return true;

        if (!(fl.size() == 1 && fl[0] == '+') && fl != topic.substr(t, te - t))
            return false;

        const bool f_more = fe < filter.size();
        const bool t_more = te < topic.size();

        if (!f_more || !t_more)
        {
            if (f_more)
            {
                const etl::string_view rest = filter.substr(fe + 1);
                return rest.size() == 1 && rest[0] == '#';
            }
            return !t_more;
        }

        f = fe + 1;
        t = te + 1;
    }
}

/// Build every string of length 1..kMaxLen over `alphabet`.
template <size_t kCap>
void enumerate(const char* alphabet, size_t n, size_t max_len,
               etl::vector<etl::string<8>, kCap>& out) noexcept
{
    etl::vector<etl::string<8>, kCap> level;
    level.push_back(etl::string<8>());

    for (size_t len = 0; len < max_len; ++len)
    {
        etl::vector<etl::string<8>, kCap> next;
        for (const etl::string<8>& s : level)
        {
            for (size_t i = 0; i < n; ++i)
            {
                if (next.full() || out.full())
                    return;
                etl::string<8> t = s;
                t.push_back(alphabet[i]);
                next.push_back(t);
                out.push_back(t);
            }
        }
        level = next;
    }
}

}   // namespace

TEST(topic_matches_agrees_with_a_reference_over_every_short_pair)
{
    // Four characters, lengths 1 to 4: 4 + 16 + 64 + 256 = 340 strings. The
    // capacity is that number rather than a round one above it, because
    // enumerate() gives its working vectors the same capacity and they sit on
    // the stack -- which ASan's larger frames make worth counting.
    constexpr size_t kMaxLen = 4;
    constexpr size_t kCap    = 340;

    static etl::vector<etl::string<8>, kCap> filters;
    static etl::vector<etl::string<8>, kCap> topics;
    filters.clear();
    topics.clear();

    enumerate("a/+#", 4, kMaxLen, filters);
    enumerate("ab/$", 4, kMaxLen, topics);

    size_t checked    = 0;
    size_t mismatches = 0;

    for (const etl::string<8>& f : filters)
    {
        const etl::string_view fv(f.data(), f.size());
        if (!is_valid_filter(fv))
            continue;

        for (const etl::string<8>& t : topics)
        {
            const etl::string_view tv(t.data(), t.size());
            if (!is_valid_topic_name(tv))
                continue;

            ++checked;

            if (topic_matches(fv, tv) != reference_matches(fv, tv))
            {
                if (mismatches < 5)
                {
                    th::report_failure("topic_matches disagrees with the reference", __FILE__,
                                       __LINE__);
                    std::printf("    filter=\"%.*s\" topic=\"%.*s\" got=%d want=%d\n",
                                static_cast<int>(f.size()), f.data(),
                                static_cast<int>(t.size()), t.data(),
                                static_cast<int>(topic_matches(fv, tv)),
                                static_cast<int>(reference_matches(fv, tv)));
                }
                ++mismatches;
            }
        }
    }

    CHECK(mismatches == 0);
    CHECK(checked > 10000);   // the sweep actually ran
}
