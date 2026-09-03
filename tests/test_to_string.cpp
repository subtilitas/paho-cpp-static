// The to_string helpers are diagnostics, so nothing else in the suite calls
// them and a missing switch case would never show up in a test run -- it would
// show up in a log, at three in the morning, as "Unknown".
//
// Each case below walks the whole enumeration and insists on a name that is
// non-empty, distinct from every other, and not the fallback. Adding an
// enumerator without extending its switch therefore fails here.

#include "test_harness.hpp"

#include <cstring>

#include "mqtt/client.hpp"
#include "mqtt/packet.hpp"

using namespace mqtt;

namespace {

/// Every name in [0, count) must be non-empty, distinct, and not `fallback`.
template <typename Enum, typename Fn>
void check_names(Fn to_name, unsigned count, const char* fallback) noexcept
{
    const char* seen[64] = {};
    REQUIRE(count <= 64);

    for (unsigned i = 0; i < count; ++i)
    {
        const char* name = to_name(static_cast<Enum>(i));

        if (!CHECK(name != nullptr) || !CHECK(name[0] != '\0'))
            continue;

        if (std::strcmp(name, fallback) == 0)
        {
            th::report_failure("enumerator has no case of its own", __FILE__, __LINE__);
            std::printf("    value %u fell through to \"%s\"\n", i, fallback);
            continue;
        }

        for (unsigned j = 0; j < i; ++j)
        {
            if (seen[j] != nullptr && std::strcmp(seen[j], name) == 0)
            {
                th::report_failure("two enumerators share a name", __FILE__, __LINE__);
                std::printf("    values %u and %u are both \"%s\"\n", j, i, name);
            }
        }
        seen[i] = name;
    }
}

}   // namespace

TEST(to_string_names_every_packet_type)
{
    // Reserved(0) through Disconnect(14).
    check_names<PacketType>([](PacketType t) { return to_string(t); }, 15u, "UNKNOWN");

    // A value off the end of the enumeration still has to be printable.
    CHECK(std::strcmp(to_string(static_cast<PacketType>(15)), "UNKNOWN") == 0);
}

TEST(to_string_names_every_connack_code)
{
    check_names<ConnackCode>([](ConnackCode c) { return to_string(c); }, 6u,
                             "Unknown CONNACK code");

    CHECK(std::strcmp(to_string(static_cast<ConnackCode>(6)), "Unknown CONNACK code") == 0);
}

TEST(to_string_names_every_error)
{
    // Ok(0) through Reentrant(22).
    check_names<Error>([](Error e) { return to_string(e); }, 23u, "Unknown");

    CHECK(std::strcmp(to_string(static_cast<Error>(23)), "Unknown") == 0);

    // Spot-check that the mapping is the right way round rather than merely
    // dense -- check_names would be satisfied by 22 distinct wrong answers.
    CHECK(std::strcmp(to_string(Error::Ok), "Ok") == 0);
    CHECK(std::strcmp(to_string(Error::TxQueueFull), "TxQueueFull") == 0);
    CHECK(std::strcmp(to_string(Error::TransportClosed), "TransportClosed") == 0);
}

TEST(to_string_names_every_state)
{
    check_names<State>([](State s) { return to_string(s); }, 5u, "Unknown");

    CHECK(std::strcmp(to_string(static_cast<State>(5)), "Unknown") == 0);
    CHECK(std::strcmp(to_string(State::Idle), "Idle") == 0);
    CHECK(std::strcmp(to_string(State::Connected), "Connected") == 0);
}

TEST(is_retryable_covers_only_the_flow_control_codes)
{
    CHECK(is_retryable(Error::WouldBlock));
    CHECK(is_retryable(Error::Incomplete));

    for (unsigned i = 0; i < 23u; ++i)
    {
        const Error e = static_cast<Error>(i);
        if (e == Error::WouldBlock || e == Error::Incomplete)
            continue;
        if (is_retryable(e))
        {
            th::report_failure("non-flow-control code reported retryable", __FILE__, __LINE__);
            std::printf("    %s\n", to_string(e));
        }
    }
}
