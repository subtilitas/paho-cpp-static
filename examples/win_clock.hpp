// SPDX-License-Identifier: MIT
//
// A monotonic millisecond clock for Windows host builds.
//
// GetTickCount64() rather than GetTickCount(): both are monotonic and both are
// millisecond resolution, but the 32-bit one wraps after 49.7 days and the
// truncation below should be the only place that happens, so that the wrap is
// the one the client already handles.
//
// QueryPerformanceCounter would give a finer resolution and is not needed --
// the client compares keep-alive and retry intervals in milliseconds.

#ifndef MQTT_WIN_CLOCK_HPP
#define MQTT_WIN_CLOCK_HPP

#if !defined(_WIN32)
#error "win_clock.hpp is for Windows; use posix_clock.hpp elsewhere"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "mqtt/transport.hpp"

namespace example {

class WinClock final : public mqtt::Clock
{
public:
    uint32_t now_ms() const noexcept override
    {
        // Truncating to 32 bits is deliberate. The client compares timestamps
        // with unsigned difference arithmetic, so the wrap every 49.7 days is
        // handled correctly and needs no special casing here.
        return static_cast<uint32_t>(::GetTickCount64());
    }
};

}   // namespace example

#endif   // MQTT_WIN_CLOCK_HPP
