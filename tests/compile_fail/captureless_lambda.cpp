// A capture-less lambda passed as a temporary. ETL's own deleted constructor
// does not catch this one -- a capture-less lambda converts to a function
// pointer, which its constraint exempts -- so mqtt::detail::Handler does.
//
// Must not compile.

#include "mqtt/client.hpp"

#include "compile_fail_fixture.hpp"

void bind_a_temporary()
{
    fixture::client().on_message([](const mqtt::Message&) { fixture::seen = 1; });
}
