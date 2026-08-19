// SPDX-License-Identifier: MIT
//
// A monotonic millisecond clock for host builds, shared by the example
// transports. On a target this is whatever your tick source already is --
// k_uptime_get_32() on Zephyr, xTaskGetTickCount() on FreeRTOS, sys_now() on
// lwIP, or a SysTick counter on bare metal.

#ifndef MQTT_POSIX_CLOCK_HPP
#define MQTT_POSIX_CLOCK_HPP

#include <ctime>

#include "mqtt/transport.hpp"

namespace example {

class PosixClock final : public mqtt::Clock
{
public:
    uint32_t now_ms() const noexcept override
    {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);

        // Truncating to 32 bits is deliberate. The client compares timestamps
        // with unsigned difference arithmetic, so the wrap every 49.7 days is
        // handled correctly and needs no special casing here.
        return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000ull +
                                     static_cast<uint64_t>(ts.tv_nsec) / 1000000ull);
    }
};

}   // namespace example

#endif   // MQTT_POSIX_CLOCK_HPP
