# A hand-rolled test harness rather than GoogleTest: the whole library is built
# with -fno-exceptions, and pulling in a framework that expects exceptions would
# undermine the thing we are trying to prove.

add_executable(mqtt_tests
    test_main.cpp
    test_packet.cpp
    test_codec.cpp
    test_topic.cpp
    test_client.cpp)

target_link_libraries(mqtt_tests PRIVATE paho_cpp_static)
target_include_directories(mqtt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT MSVC)
    target_compile_options(mqtt_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mqtt_tests COMMAND mqtt_tests)

# Separate registration so CI can exclude the allocation probe under ASan, whose
# allocator interception conflicts with replacing global operator new.
set_tests_properties(mqtt_tests PROPERTIES LABELS "unit")
