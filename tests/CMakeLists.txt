# A hand-rolled test harness rather than GoogleTest: the whole library is built
# with -fno-exceptions, and pulling in a framework that expects exceptions would
# undermine the thing we are trying to prove.

add_executable(mqtt_tests
    test_main.cpp
    test_packet.cpp
    test_codec.cpp
    test_topic.cpp
    test_utf8.cpp
    test_client.cpp
    test_callback_reentrancy.cpp
    test_to_string.cpp
    test_error_values.cpp
    test_tx_queue.cpp
    test_encode_bounds.cpp
    test_config_profiles.cpp
    test_step_bounds.cpp
    test_no_alloc.cpp
    test_wraparound.cpp)

target_link_libraries(mqtt_tests PRIVATE paho_cpp_static)
target_include_directories(mqtt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT MSVC)
    target_compile_options(mqtt_tests PRIVATE -Wall -Wextra -Wpedantic)

    # Under MQTT_WERROR the tests are held to the same standard as the library.
    # -Wswitch caught a test whose handler silently did nothing -- an enumerator
    # missing from a switch -- and a warning in a wall of build output is easy
    # to scroll past. A failed build is not.
    if(MQTT_WERROR)
        target_compile_options(mqtt_tests PRIVATE -Werror)
    endif()
endif()

add_test(NAME mqtt_tests COMMAND mqtt_tests)

# Separate registration so CI can exclude the allocation probe under ASan, whose
# allocator interception conflicts with replacing global operator new.
set_tests_properties(mqtt_tests PROPERTIES LABELS "unit")

#-------------------------------------------------------------------------------
# Compile-failure tests for the callback guard
#
# mqtt::detail::Handler refuses a temporary callable, because the delegate it
# wraps stores a pointer to the callable rather than owning it. That guard
# cannot be exercised from a normal test: what it produces is a build error,
# not a wrong answer. So each case is its own target, kept out of `all`, and
# CTest runs it by building it.
#
# OBJECT libraries rather than executables: these only ever need to compile,
# and the positive control has no main() to link.
#-------------------------------------------------------------------------------
function(mqtt_add_compile_case name must_fail)
    add_library(mqtt_case_${name} OBJECT EXCLUDE_FROM_ALL compile_fail/${name}.cpp)

    # Headers and ETL, but deliberately not paho_cpp_static. Linking it would
    # inherit its PUBLIC options, and under MQTT_COVERAGE those instrument this
    # translation unit -- which is compiled and never run, so every template it
    # instantiates would be counted as uncovered and the reported figure would
    # drop by a third for no change in what the suite actually exercises.
    target_link_libraries(mqtt_case_${name} PRIVATE mqtt_etl)
    target_include_directories(mqtt_case_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail)
    target_compile_features(mqtt_case_${name} PRIVATE cxx_std_17)

    add_test(NAME compile_${name}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target mqtt_case_${name} --config $<CONFIG>)
    set_tests_properties(compile_${name} PROPERTIES
                         WILL_FAIL ${must_fail}
                         LABELS "compile-fail")
endfunction()

# A temporary callable must be rejected. ETL's own deleted constructor catches
# the capturing case; it exempts anything convertible to a function pointer,
# which is why the capture-less case needs Handler.
mqtt_add_compile_case(capturing_lambda   TRUE)
mqtt_add_compile_case(captureless_lambda TRUE)

# The positive control. Without it, a Handler that rejected every callable --
# including the ones it must accept -- would satisfy both cases above.
mqtt_add_compile_case(named_lambda FALSE)

#-------------------------------------------------------------------------------
# The interop broker
#
# A second MQTT 3.1.1 implementation to test the client against. It links
# nothing from this project on purpose -- an interop partner that shared code
# with the client under test would agree with it about any shared mistake.
#
# Built only when pointed at an unpacked minimosq headers release. SYSTEM
# include because those headers are not ours to keep warning-clean under this
# project's -Wconversion pass.
#-------------------------------------------------------------------------------
if(MQTT_MINIMOSQ_DIR AND UNIX)
    add_executable(minimosq_broker interop/minimosq_broker.cpp)
    target_include_directories(minimosq_broker SYSTEM PRIVATE "${MQTT_MINIMOSQ_DIR}/include")
    target_compile_features(minimosq_broker PRIVATE cxx_std_17)
    set_target_properties(minimosq_broker PROPERTIES CXX_EXTENSIONS OFF)
endif()

#-------------------------------------------------------------------------------
# Fuzz targets
#
# Everything a peer controls arrives as a byte stream, and the unit suite tests
# the malformed inputs somebody thought to write down. These cover the ones
# nobody did.
#
# libFuzzer only, so clang only. The sanitizers are not optional here: without
# them the fuzzer finds a crash and nothing else, and an out-of-bounds read
# that does not segfault is exactly the bug worth finding.
#-------------------------------------------------------------------------------
if(MQTT_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "MQTT_FUZZ needs clang for -fsanitize=fuzzer; "
            "configure with -DCMAKE_CXX_COMPILER=clang++")
    endif()

    foreach(target fuzz_codec fuzz_client)
        add_executable(${target} fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE paho_cpp_static)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endforeach()

    # A short deterministic run over the checked-in corpus, so a regression is
    # caught by the ordinary test suite rather than only by a long fuzz job.
    # -runs is a count, not a clock, so it does not vary with runner load.
    foreach(target fuzz_codec fuzz_client)
        add_test(NAME ${target}_corpus
                 COMMAND ${target} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${target}
                         -runs=2000 -timeout=10 -error_exitcode=1)
        set_tests_properties(${target}_corpus PROPERTIES LABELS "fuzz")
    endforeach()
endif()
