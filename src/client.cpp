// SPDX-License-Identifier: MIT

#include "mqtt/client.hpp"

namespace mqtt {

const char* to_string(State s) noexcept
{
    switch (s)
    {
        case State::Idle:            return "Idle";
        case State::Connecting:      return "Connecting";
        case State::AwaitingConnack: return "AwaitingConnack";
        case State::Connected:       return "Connected";
        case State::Disconnecting:   return "Disconnecting";
    }
    return "Unknown";
}

} // namespace mqtt
