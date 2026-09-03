# Host-only examples. The library itself has no OS dependency; these show three
# ways to satisfy the Transport interface.
#
#   minimal_publisher  smallest complete program, resolves a hostname
#   pubsub_demo        publish and subscribe at all three QoS levels
#   ip_publisher       fixed IPv4 address, no DNS -- closest to a shipped device

# ip_publisher builds on both platforms: it selects winsock_transport.hpp and
# win_clock.hpp on Windows and the POSIX pair elsewhere. The other two use
# posix_transport.hpp, which calls getaddrinfo() through <netdb.h>.
set(mqtt_demos ip_publisher)
if(UNIX)
    list(APPEND mqtt_demos minimal_publisher pubsub_demo)
endif()

foreach(demo ${mqtt_demos})
    add_executable(${demo} ${demo}.cpp)
    target_link_libraries(${demo} PRIVATE paho_cpp_static)
    target_include_directories(${demo} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(WIN32)
        # Winsock. Nothing in the library needs it; the transport does.
        target_link_libraries(${demo} PRIVATE ws2_32)
    endif()

    if(NOT MSVC)
        target_compile_options(${demo} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# tls_transport.hpp is header-only and needs mbedTLS to compile, so it is not
# built here. Define MQTT_EXAMPLE_USE_MBEDTLS and link mbedtls to use it.
