// The numeric value of every Error is part of the public API, so it is pinned
// here rather than left to declaration order.
//
// Removing an enumerator renumbers every one after it. That is invisible at the
// call site -- code comparing against the names keeps compiling and keeps
// working -- and it silently changes what an application persisted to flash or
// put on a wire in an earlier build. It has happened once: 0.6.0 removed
// NoInboundSlot and shifted nine codes down by one.
//
// So the table below is the contract. Under the 1.x promise a retired code
// keeps its number and its slot, and the number is never reused. Add to the
// end; never renumber.

#include "test_harness.hpp"

#include <cstring>

#include "mqtt/error.hpp"

using namespace mqtt;

namespace {

struct Pin
{
    Error       code;
    uint8_t     value;
    const char* name;
};

/// The frozen numbering, in order. This is the declaration the rest of the
/// world depends on.
constexpr Pin kPinned[] = {
    {Error::Ok, 0, "Ok"},

    {Error::WouldBlock, 1, "WouldBlock"},
    {Error::Incomplete, 2, "Incomplete"},

    {Error::InvalidArgument, 3, "InvalidArgument"},
    {Error::NotConnected, 4, "NotConnected"},
    {Error::AlreadyConnected, 5, "AlreadyConnected"},
    {Error::NotSupported, 6, "NotSupported"},

    {Error::BufferTooSmall, 7, "BufferTooSmall"},
    {Error::TxQueueFull, 8, "TxQueueFull"},
    {Error::NoInflightSlot, 9, "NoInflightSlot"},
    {Error::NoSubscriptionSlot, 10, "NoSubscriptionSlot"},
    {Error::NoPendingAckSlot, 11, "NoPendingAckSlot"},
    {Error::PayloadTooLarge, 12, "PayloadTooLarge"},
    {Error::TopicTooLong, 13, "TopicTooLong"},

    {Error::MalformedPacket, 14, "MalformedPacket"},
    {Error::ProtocolViolation, 15, "ProtocolViolation"},
    {Error::PacketTooLarge, 16, "PacketTooLarge"},
    {Error::ConnectionRefused, 17, "ConnectionRefused"},
    {Error::KeepAliveTimeout, 18, "KeepAliveTimeout"},
    {Error::ConnectTimeout, 19, "ConnectTimeout"},

    {Error::TransportFailure, 20, "TransportFailure"},
    {Error::TransportClosed, 21, "TransportClosed"},

    {Error::Reentrant, 22, "Reentrant"},
};

constexpr unsigned kCount = static_cast<unsigned>(sizeof(kPinned) / sizeof(kPinned[0]));

/// Checked at compile time, so a renumber cannot be skipped by not running the
/// suite -- it fails the build of anything that includes this file.
constexpr bool values_agree() noexcept
{
    for (const Pin& p : kPinned)
    {
        if (static_cast<uint8_t>(p.code) != p.value)
            return false;
    }
    return true;
}

/// Dense and strictly increasing from zero. Catches a duplicate value and a
/// gap, neither of which values_agree() alone would notice.
constexpr bool numbering_is_dense() noexcept
{
    for (unsigned i = 0; i < kCount; ++i)
    {
        if (kPinned[i].value != static_cast<uint8_t>(i))
            return false;
    }
    return true;
}

static_assert(values_agree(), "an Error enumerator changed its numeric value");
static_assert(numbering_is_dense(), "the Error numbering has a gap or a duplicate");
static_assert(sizeof(Error) == 1, "Error is documented as one byte");

}   // namespace

TEST(error_values_are_pinned)
{
    // The compile-time assertions above are the real gate. These restate them
    // at run time so the pin is visible in the suite's output, and name the
    // enumerator that moved rather than only failing to compile.
    for (unsigned i = 0; i < kCount; ++i)
    {
        const Pin& p = kPinned[i];

        if (!CHECK(static_cast<uint8_t>(p.code) == p.value))
            std::printf("    %s is %u, pinned at %u\n", p.name, static_cast<unsigned>(p.code),
                        static_cast<unsigned>(p.value));
    }
}

TEST(error_names_match_the_pinned_table)
{
    // A rename is a source-breaking change too, and to_string() is what a log
    // shows. Pinning the spelling keeps the two in step.
    for (unsigned i = 0; i < kCount; ++i)
    {
        const Pin& p = kPinned[i];

        if (!CHECK(std::strcmp(to_string(p.code), p.name) == 0))
            std::printf("    value %u prints as \"%s\", pinned as \"%s\"\n", p.value,
                        to_string(p.code), p.name);
    }
}

TEST(error_table_covers_the_whole_enumeration)
{
    // Guards the other direction: an enumerator added to error.hpp without a
    // row here. to_string() has a case for every real code and returns the
    // fallback otherwise, so the first unnamed value marks the end.
    CHECK(kCount == 23u);
    CHECK(std::strcmp(to_string(static_cast<Error>(kCount)), "Unknown") == 0);

    // is_retryable() is part of the same contract -- it is spelled in terms of
    // two specific codes, so a renumber that slipped past the table would show
    // up as the wrong code being retried.
    CHECK(is_retryable(Error::WouldBlock));
    CHECK(is_retryable(Error::Incomplete));
    CHECK(!is_retryable(Error::Ok));
    CHECK(!is_retryable(Error::TransportClosed));
}
