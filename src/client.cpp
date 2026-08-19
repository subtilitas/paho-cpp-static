// SPDX-License-Identifier: MIT

#include "mqtt/client.hpp"

namespace mqtt {

const char* to_string(State s) noexcept
{
    // The returns are aligned into a column on purpose; clang-format has no
    // option that preserves it, so it is switched off across the table.
    // clang-format off
    switch (s)
    {
        case State::Idle:            return "Idle";
        case State::Connecting:      return "Connecting";
        case State::AwaitingConnack: return "AwaitingConnack";
        case State::Connected:       return "Connected";
        case State::Disconnecting:   return "Disconnecting";
    }
    // clang-format on
    return "Unknown";
}

}   // namespace mqtt
