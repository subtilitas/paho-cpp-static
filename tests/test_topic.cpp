#include "test_harness.hpp"

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
