# Host-only examples. The library itself has no OS dependency; these show three
# ways to satisfy the Transport interface.
#
#   minimal_publisher  smallest complete program, resolves a hostname
#   pubsub_demo        publish and subscribe at all three QoS levels
#   ip_publisher       fixed IPv4 address, no DNS -- closest to a shipped device

foreach(demo minimal_publisher pubsub_demo ip_publisher)
    add_executable(${demo} ${demo}.cpp)
    target_link_libraries(${demo} PRIVATE paho_cpp_static)
    target_include_directories(${demo} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(NOT MSVC)
        target_compile_options(${demo} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# tls_transport.hpp is header-only and needs mbedTLS to compile, so it is not
# built here. Define MQTT_EXAMPLE_USE_MBEDTLS and link mbedtls to use it.
