// The positive control: a named callable whose lifetime the caller owns.
//
// Must compile. Without this case a Handler that rejected every callable would
// satisfy the two negative tests while being useless.

#include "mqtt/client.hpp"

#include "compile_fail_fixture.hpp"

void bind_a_named_callable()
{
    static auto handler = [](const mqtt::Message&) { fixture::seen = 1; };
    fixture::client().on_message(handler);

    // A per-subscription handler takes the same slot type, so it is guarded the
    // same way and must accept the same lvalue.
    fixture::client().subscribe(etl::string_view("a/b"), mqtt::QoS::AtMostOnce, handler);

    // Binding a member function goes through the underlying delegate's
    // create(). It must stay reachable: it is how a class-based port hooks
    // itself up, and it produces a delegate rather than a callable, so the
    // temporary guard must not reject it.
    using H = mqtt::Client<mqtt::DefaultConfig>::MessageHandler;
    fixture::client().on_message(
        H::Delegate::create<fixture::Sink, &fixture::Sink::on_message>(fixture::sink()));
}
