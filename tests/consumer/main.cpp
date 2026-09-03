// Compiles against the installed headers and links the installed archive.
//
// It instantiates a client rather than only including a header, because the
// ETL include path is what an exported config is most likely to get wrong and
// nothing in mqtt/client.hpp needs ETL until a template is instantiated.

#include <cstdio>

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
        std::printf("FAIL: TransportClosed is %u, expected 21\n",
                    static_cast<unsigned>(mqtt::Error::TransportClosed));
        return 1;
    }

    std::printf("ok: sizeof(Client<DefaultConfig>) = %zu, TransportClosed = %s\n",
                sizeof(mqtt::Client<Cfg>), mqtt::to_string(mqtt::Error::TransportClosed));
    return 0;
}
