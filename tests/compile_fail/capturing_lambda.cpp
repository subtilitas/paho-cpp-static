// A capturing lambda passed as a temporary. The delegate would store a pointer
// to a closure that dies at the end of this statement.
//
// Must not compile.

#include "mqtt/client.hpp"

#include "compile_fail_fixture.hpp"

void bind_a_temporary()
{
    int n = 0;
    fixture::client().on_message([&n](const mqtt::Message&) { n = 1; });
}
