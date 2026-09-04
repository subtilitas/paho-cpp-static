// Test doubles for Transport and Clock.
//
// Backed by fixed-capacity etl::vectors rather than std::vector so that the
// zero-allocation test measures the client and not the harness.

#ifndef MQTT_TEST_FAKES_HPP
#define MQTT_TEST_FAKES_HPP

#include <cstdint>

#include <etl/vector.h>

#include "mqtt/transport.hpp"

namespace fakes {

constexpr size_t kPipeCapacity = 4096;

/// An in-memory transport with controllable back-pressure.
class FakeTransport final : public mqtt::Transport
{
public:
    // --- knobs ---------------------------------------------------------------

    /// Number of connect() calls that report WouldBlock before succeeding.
    int connect_delay = 0;
    /// Fail the next connect() outright.
    bool connect_fails = false;
    /// Maximum bytes accepted per send() call. 0 means unlimited.
    size_t send_limit = 0;
    /// Maximum bytes produced per recv() call. 0 means unlimited.
    size_t recv_limit = 0;
    /// Make send() report WouldBlock without consuming anything.
    bool send_blocked = false;
    /// Make recv() report TransportClosed once the inbound pipe is drained.
    bool close_when_drained = false;

    // --- Transport -----------------------------------------------------------

    mqtt::Error connect() noexcept override
    {
        ++connect_calls;
        if (connect_fails)
            return mqtt::Error::TransportFailure;
        if (connect_delay > 0)
        {
            --connect_delay;
            return mqtt::Error::WouldBlock;
        }
        connected_ = true;
        return mqtt::Error::Ok;
    }

    mqtt::Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = 0;
        if (!connected_)
            return mqtt::Error::TransportClosed;
        if (send_blocked)
            return mqtt::Error::WouldBlock;

        size_t n = data.size();
        if (send_limit != 0 && n > send_limit)
            n = send_limit;
        if (sent.size() + n > sent.max_size())
            n = sent.max_size() - sent.size();

        for (size_t i = 0; i < n; ++i)
            sent.push_back(data[i]);

        written = n;
        return mqtt::Error::Ok;
    }

    mqtt::Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (!connected_)
            return mqtt::Error::TransportClosed;

        const size_t available = inbound.size() - inbound_pos_;
        if (available == 0)
            return close_when_drained ? mqtt::Error::TransportClosed : mqtt::Error::WouldBlock;

        size_t n = available;
        if (n > buffer.size())
            n = buffer.size();
        if (recv_limit != 0 && n > recv_limit)
            n = recv_limit;

        for (size_t i = 0; i < n; ++i)
            buffer[i] = inbound[inbound_pos_ + i];

        inbound_pos_ += n;
        read = n;
        return mqtt::Error::Ok;
    }

    void close() noexcept override
    {
        connected_ = false;
        ++close_calls;
    }

    bool is_connected() const noexcept override { return connected_; }

    // --- inspection ----------------------------------------------------------

    /// Feed bytes to the client as if the broker had sent them.
    void push_inbound(const uint8_t* data, size_t len) noexcept
    {
        for (size_t i = 0; i < len && inbound.size() < inbound.max_size(); ++i)
            inbound.push_back(data[i]);
    }

    void push_inbound(etl::span<const uint8_t> data) noexcept
    {
        push_inbound(data.data(), data.size());
    }

    /// Drop everything the client has sent so far.
    void clear_sent() noexcept { sent.clear(); }

    /// Drop everything queued for the client and reset the read cursor. The
    /// inbound pipe only ever grows as it is consumed, so a test that drives
    /// many round trips has to reclaim it or it fills at kPipeCapacity.
    void clear_inbound() noexcept
    {
        inbound.clear();
        inbound_pos_ = 0;
    }

    /// Force the connection up without going through connect().
    void force_connected() noexcept { connected_ = true; }

    etl::vector<uint8_t, kPipeCapacity> sent{};      ///< bytes the client wrote
    etl::vector<uint8_t, kPipeCapacity> inbound{};   ///< bytes queued for the client

    int connect_calls = 0;
    int close_calls   = 0;

private:
    bool   connected_   = false;
    size_t inbound_pos_ = 0;
};

/// A manually advanced millisecond clock.
class FakeClock final : public mqtt::Clock
{
public:
    uint32_t now_ms() const noexcept override { return now; }

    void advance(uint32_t ms) noexcept { now += ms; }

    uint32_t now = 0;
};

}   // namespace fakes

#endif   // MQTT_TEST_FAKES_HPP
