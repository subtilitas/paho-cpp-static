// TxQueue is a fixed-capacity byte FIFO with hand-rolled index arithmetic and
// a memmove, driven indirectly by every client test but asserted on by none of
// them. Compaction in particular only runs when the queue is partially drained
// and then asked for more room than remains at the tail, which the client
// reaches rarely and never checks the contents afterwards.

#include "test_harness.hpp"

#include <cstring>

#include "mqtt/tx_queue.hpp"

using namespace mqtt;

namespace {

/// Append `text` through the reserve/write_span/commit sequence callers use.
bool push(TxQueue<16>& q, const char* text) noexcept
{
    const size_t n = std::strlen(text);
    if (!q.reserve(n))
        return false;
    std::memcpy(q.write_span().data(), text, n);
    q.commit(n);
    return true;
}

bool peek_equals(const TxQueue<16>& q, const char* text) noexcept
{
    const etl::span<const uint8_t> s = q.peek();
    const size_t                   n = std::strlen(text);
    return s.size() == n && std::memcmp(s.data(), text, n) == 0;
}

}   // namespace

TEST(tx_queue_starts_empty)
{
    TxQueue<16> q;
    CHECK(q.empty());
    CHECK_EQ(q.pending(), size_t{0});
    CHECK_EQ(TxQueue<16>::capacity(), size_t{16});
    CHECK_EQ(q.peek().size(), size_t{0});
}

TEST(tx_queue_round_trips_bytes_in_order)
{
    TxQueue<16> q;
    CHECK(push(q, "abc"));
    CHECK(push(q, "de"));

    CHECK_EQ(q.pending(), size_t{5});
    CHECK(!q.empty());
    CHECK(peek_equals(q, "abcde"));

    q.consume(2);
    CHECK_EQ(q.pending(), size_t{3});
    CHECK(peek_equals(q, "cde"));

    q.consume(3);
    CHECK(q.empty());
}

TEST(tx_queue_refuses_more_than_capacity)
{
    TxQueue<16> q;
    CHECK(!q.reserve(17));
    CHECK(q.reserve(16));

    // A refused reserve must not have disturbed anything.
    CHECK(q.empty());
}

TEST(tx_queue_reports_full_when_it_is)
{
    TxQueue<16> q;
    CHECK(push(q, "0123456789abcdef"));   // exactly 16
    CHECK_EQ(q.pending(), size_t{16});
    CHECK(!q.reserve(1));
}

// The case the client hits under a slow transport: some bytes have gone out,
// the rest have not, and the next packet does not fit in what is left at the
// tail. compact() memmoves the unsent remainder to the front.
TEST(tx_queue_compacts_to_make_room_and_keeps_the_unsent_bytes)
{
    TxQueue<16> q;
    CHECK(push(q, "0123456789ab"));   // 12 of 16 used, 4 left at the tail
    q.consume(10);                    // 2 unsent ("ab"), but still at offset 10

    CHECK_EQ(q.pending(), size_t{2});
    CHECK(peek_equals(q, "ab"));

    // 8 does not fit in the 4 bytes left at the tail, but does fit once the
    // two unsent bytes move to the front.
    CHECK(q.reserve(8));
    CHECK(peek_equals(q, "ab"));   // survived the move, and is still first

    CHECK(push(q, "XYZ"));
    CHECK_EQ(q.pending(), size_t{5});
    CHECK(peek_equals(q, "abXYZ"));
}

TEST(tx_queue_compaction_cannot_conjure_space)
{
    TxQueue<16> q;
    CHECK(push(q, "0123456789ab"));
    q.consume(2);   // 10 unsent, 6 free in total

    CHECK(!q.reserve(7));   // more than the free space, even compacted
    CHECK(q.reserve(6));
    CHECK(peek_equals(q, "23456789ab"));
}

// Draining to empty resets the indices, which is what keeps compaction off the
// steady-state path entirely.
TEST(tx_queue_resets_to_the_front_once_drained)
{
    TxQueue<16> q;
    CHECK(push(q, "0123456789"));
    q.consume(10);
    CHECK(q.empty());

    // The full capacity is available again with no compaction needed.
    CHECK(q.reserve(16));
    CHECK(push(q, "0123456789abcdef"));
    CHECK_EQ(q.pending(), size_t{16});
}

TEST(tx_queue_clamps_oversized_commit_and_consume)
{
    TxQueue<16> q;
    CHECK(q.reserve(4));
    std::memcpy(q.write_span().data(), "abcd", 4);

    // Committing more than the buffer holds must saturate, not run past it.
    q.commit(999);
    CHECK_EQ(q.pending(), TxQueue<16>::capacity());

    // Likewise consuming more than is pending.
    q.consume(999);
    CHECK(q.empty());
    CHECK_EQ(q.pending(), size_t{0});
}

TEST(tx_queue_clear_discards_everything)
{
    TxQueue<16> q;
    CHECK(push(q, "abcd"));
    q.consume(1);
    q.clear();

    CHECK(q.empty());
    CHECK_EQ(q.pending(), size_t{0});
    CHECK(q.reserve(16));   // and the whole capacity is back
}
