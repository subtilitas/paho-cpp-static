// The to_string helpers are diagnostics, so nothing else in the suite calls
// them and a missing switch case would never show up in a test run -- it would
// show up in a log, at three in the morning, as "Unknown".
//
// Each case below walks the whole enumeration and insists on a name that is
// non-empty, distinct from every other, and not the fallback. Adding an
// enumerator without extending its switch therefore fails here.

#include "test_harness.hpp"

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

        if (th::same_text(name, fallback))
        {
            th::report_failure("enumerator has no case of its own", __FILE__, __LINE__);
            std::cout << "    value " << i << " fell through to \"" << fallback << "\"\n";
            continue;
        }

        for (unsigned j = 0; j < i; ++j)
        {
            if (seen[j] != nullptr && th::same_text(seen[j], name))
            {
                th::report_failure("two enumerators share a name", __FILE__, __LINE__);
                std::cout << "    values " << j << " and " << i << " are both \"" << name
                          << "\"\n";
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
    CHECK(th::same_text(to_string(static_cast<PacketType>(15)), "UNKNOWN"));
}

TEST(to_string_names_every_connack_code)
{
    check_names<ConnackCode>([](ConnackCode c) { return to_string(c); }, 6u,
                             "Unknown CONNACK code");

    CHECK(th::same_text(to_string(static_cast<ConnackCode>(6)), "Unknown CONNACK code"));
}

TEST(to_string_names_every_error)
{
    // Ok(0) through Reentrant(22).
    check_names<Error>([](Error e) { return to_string(e); }, 23u, "Unknown");

    CHECK(th::same_text(to_string(static_cast<Error>(23)), "Unknown"));

    // Spot-check that the mapping is the right way round rather than merely
    // dense -- check_names would be satisfied by 22 distinct wrong answers.
    CHECK(th::same_text(to_string(Error::Ok), "Ok"));
    CHECK(th::same_text(to_string(Error::TxQueueFull), "TxQueueFull"));
    CHECK(th::same_text(to_string(Error::TransportClosed), "TransportClosed"));
}

TEST(to_string_names_every_state)
{
    check_names<State>([](State s) { return to_string(s); }, 5u, "Unknown");

    CHECK(th::same_text(to_string(static_cast<State>(5)), "Unknown"));
    CHECK(th::same_text(to_string(State::Idle), "Idle"));
    CHECK(th::same_text(to_string(State::Connected), "Connected"));
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
            std::cout << "    " << to_string(e) << '\n';
        }
    }
}
