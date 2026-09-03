// Shared scaffolding for the compile-failure cases. Nothing here runs -- these
// targets are built, never executed -- so the client is only ever declared.

#ifndef MQTT_COMPILE_FAIL_FIXTURE_HPP
#define MQTT_COMPILE_FAIL_FIXTURE_HPP

#include "mqtt/client.hpp"

namespace fixture {

inline int seen = 0;

/// Declared, not defined. Each case only needs a client to name.
mqtt::Client<mqtt::DefaultConfig>& client() noexcept;

/// A receiver with a member function, for the delegate's create().
struct Sink
{
    void on_message(const mqtt::Message&) noexcept;
};

Sink& sink() noexcept;

}   // namespace fixture

#endif   // MQTT_COMPILE_FAIL_FIXTURE_HPP
