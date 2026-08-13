// SPDX-License-Identifier: MIT
//
// A fixed-capacity byte FIFO for outbound packets.
//
// Deliberately linear rather than circular: the transport wants one contiguous
// span per send() call, and a circular buffer would either hand out two spans
// or force packets to straddle the wrap point. Compaction is a memmove of at
// most the unsent bytes, which in steady state is zero because the queue drains
// to empty and resets its indices.

#ifndef MQTT_TX_QUEUE_HPP
#define MQTT_TX_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <etl/span.h>

namespace mqtt {

template <size_t Capacity>
class TxQueue
{
public:
    static constexpr size_t capacity() noexcept { return Capacity; }

    /// Bytes written but not yet handed to the transport.
    size_t pending() const noexcept { return tail_ - head_; }

    bool empty() const noexcept { return head_ == tail_; }

    /// Make room for `n` contiguous bytes at the tail, compacting if that is
    /// what it takes. Returns false if `n` cannot fit even after compaction.
    bool reserve(size_t n) noexcept
    {
        if (n > Capacity)
            return false;

        if (Capacity - tail_ >= n)
            return true;   // already room at the tail

        compact();
        return (Capacity - tail_) >= n;
    }

    /// Writable region at the tail. Call reserve() first.
    etl::span<uint8_t> write_span() noexcept
    {
        return etl::span<uint8_t>(buffer_ + tail_, Capacity - tail_);
    }

    /// Publish `n` bytes previously written into write_span().
    void commit(size_t n) noexcept
    {
        const size_t room = Capacity - tail_;
        tail_ += (n < room) ? n : room;
    }

    /// The bytes waiting to go out, as one contiguous span.
    etl::span<const uint8_t> peek() const noexcept
    {
        return etl::span<const uint8_t>(buffer_ + head_, tail_ - head_);
    }

    /// Drop `n` bytes from the front after the transport accepted them.
    void consume(size_t n) noexcept
    {
        const size_t avail = tail_ - head_;
        head_ += (n < avail) ? n : avail;

        if (head_ == tail_)
        {
            head_ = 0;
            tail_ = 0;   // steady-state reset: compaction never runs
        }
    }

    void clear() noexcept
    {
        head_ = 0;
        tail_ = 0;
    }

private:
    void compact() noexcept
    {
        if (head_ == 0)
            return;

        const size_t n = tail_ - head_;
        if (n > 0)
            std::memmove(buffer_, buffer_ + head_, n);

        head_ = 0;
        tail_ = n;
    }

    uint8_t buffer_[Capacity] = {};
    size_t  head_             = 0;
    size_t  tail_             = 0;
};

} // namespace mqtt

#endif // MQTT_TX_QUEUE_HPP
