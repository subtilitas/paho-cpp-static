// Compiles against the installed headers and links the installed archive.
//
// It instantiates a client rather than only including a header, because the
// ETL include path is what an exported config is most likely to get wrong and
// nothing in mqtt/client.hpp needs ETL until a template is instantiated.

#include <iostream>

#include "mqtt/client.hpp"

namespace {

struct Cfg : mqtt::DefaultConfig
{
};

}   // namespace

int main()
{
    // Reaches ETL through the package's own include path.
    static_assert(sizeof(mqtt::Client<Cfg>) > 0, "the client failed to instantiate");

    // The pinned Error numbering is part of what the package promises.
    if (static_cast<unsigned>(mqtt::Error::TransportClosed) != 21u)
    {
        std::cout << "FAIL: TransportClosed is "
                  << static_cast<unsigned>(mqtt::Error::TransportClosed) << ", expected 21\n";
        return 1;
    }

    std::cout << "ok: sizeof(Client<DefaultConfig>) = " << sizeof(mqtt::Client<Cfg>)
              << ", TransportClosed = " << mqtt::to_string(mqtt::Error::TransportClosed)
              << '\n';
    return 0;
}
