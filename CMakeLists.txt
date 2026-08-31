cmake_minimum_required(VERSION 3.16)
project(paho-cpp-static VERSION 0.5.0 LANGUAGES CXX
        DESCRIPTION "Statically allocated MQTT 3.1.1 client in C++17")

#-------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------
option(MQTT_BUILD_TESTS    "Build the unit tests"           ON)
option(MQTT_BUILD_EXAMPLES "Build the host examples"        ON)
option(MQTT_NO_EXCEPTIONS  "Compile with -fno-exceptions"   ON)
option(MQTT_NO_RTTI        "Compile with -fno-rtti"         ON)
option(MQTT_WERROR         "Treat warnings as errors"       OFF)
option(MQTT_COVERAGE       "Instrument for gcov/llvm-cov"   OFF)
option(MQTT_STACK_USAGE    "Emit GCC .su stack-usage reports" OFF)

set(MQTT_ETL_DIR "" CACHE PATH
    "Path to an Embedded Template Library checkout. If empty, ETL is fetched.")

# The dependency pin, in one place. tools/pins.py reads these and checks that
# every workflow cache key agrees, so the version cannot drift between here and
# CI without the build saying so.
set(MQTT_ETL_TAG "20.39.4" CACHE STRING
    "ETL release that MQTT_ETL_COMMIT corresponds to. Documentation for humans.")
set(MQTT_ETL_COMMIT "081e920302e4062dbd122fc3c86255825ccaa666" CACHE STRING
    "Exact ETL commit to fetch. This is the pin; the tag above only names it.")

# Treat this as a subproject when someone add_subdirectory()s us.
if(NOT CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(MQTT_BUILD_TESTS OFF)
    set(MQTT_BUILD_EXAMPLES OFF)
endif()

#-------------------------------------------------------------------------------
# Exception and RTTI policy
#
# GCC and Clang take a flag to switch these off. MSVC does not: CMake puts
# /EHsc into CMAKE_CXX_FLAGS by default, and adding /EHs-c- on top of it makes
# cl.exe emit D9025 ("overriding /EHsc") rather than doing what you meant. The
# only clean way is to remove the flag before any target sees it, which has to
# happen here at directory scope rather than on a target.
#
# _HAS_EXCEPTIONS=0 is the companion: it stops the MSVC standard library from
# emitting throw sites of its own. It is viral -- every translation unit linked
# together has to agree on it -- which is why it goes on the target as PUBLIC
# below. A consumer who wants exceptions should set MQTT_NO_EXCEPTIONS=OFF
# rather than fight it.
#-------------------------------------------------------------------------------
if(MSVC)
    if(MQTT_NO_EXCEPTIONS)
        string(REGEX REPLACE "/EH[a-zA-Z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
    endif()
    if(MQTT_NO_RTTI)
        string(REGEX REPLACE "(^| )/GR( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
    endif()
endif()

#-------------------------------------------------------------------------------
# Embedded Template Library
#-------------------------------------------------------------------------------
add_library(mqtt_etl INTERFACE)

# ETL works out the native byte order from compiler macros, but the ones it
# looks for (__BYTE_ORDER__, __ORDER_LITTLE_ENDIAN__) are a GCC/Clang thing.
# MSVC defines none of them, and below C++20 there is no std::endian to fall
# back on, so etl/endianness.h stops the build with an #error. Detect the
# target's byte order with CMake instead and hand ETL the answer.
#
# Detection is of the *target*, so this stays correct when cross-compiling to a
# big-endian part. ETL uses a pre-defined ETL_ENDIAN_NATIVE verbatim, with its
# own convention of 0 for little endian and 1 for big.
if(DEFINED CMAKE_CXX_BYTE_ORDER)
    # CMake 3.20 and newer know the answer already.
    if(CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
        set(_mqtt_endian_default 1)
    else()
        set(_mqtt_endian_default 0)
    endif()
else()
    include(TestBigEndian)
    test_big_endian(_mqtt_target_is_big_endian)
    if(_mqtt_target_is_big_endian)
        set(_mqtt_endian_default 1)
    else()
        set(_mqtt_endian_default 0)
    endif()
endif()

set(MQTT_ETL_ENDIAN_NATIVE "${_mqtt_endian_default}" CACHE STRING
    "Value for ETL_ENDIAN_NATIVE: 0 for little endian, 1 for big endian.")
set_property(CACHE MQTT_ETL_ENDIAN_NATIVE PROPERTY STRINGS 0 1)

target_compile_definitions(mqtt_etl INTERFACE
    ETL_ENDIAN_NATIVE=${MQTT_ETL_ENDIAN_NATIVE})

if(MQTT_ETL_DIR)
    # SYSTEM keeps our -Wconversion/-Wold-style-cast pass from drowning in
    # diagnostics about a third-party header we do not maintain.
    target_include_directories(mqtt_etl SYSTEM INTERFACE "${MQTT_ETL_DIR}/include")
else()
    include(FetchContent)

    # Pinned by commit, not by tag.
    #
    # A tag is a mutable ref. The same name can be moved to different content,
    # and nothing in a build that only records "20.39.4" would notice -- which
    # makes the build neither reproducible nor auditable, and is exactly the
    # supply-chain question an integrator has to answer about every dependency
    # they ship. The commit is immutable; the tag is kept alongside it because
    # a bare hash tells a human nothing.
    #
    # .github/workflows/supply-chain.yml checks weekly that this commit is
    # still the one MQTT_ETL_TAG names. A disagreement means the tag moved,
    # which is worth being woken up for.
    #
    # No GIT_SHALLOW: fetching an arbitrary commit shallowly requires the
    # remote to permit it, which is not something to depend on. The CI cache
    # means the full clone happens once per key rather than once per job.
    FetchContent_Declare(etl
        GIT_REPOSITORY https://github.com/ETLCPP/etl.git
        GIT_TAG        ${MQTT_ETL_COMMIT})
    FetchContent_MakeAvailable(etl)
    get_target_property(_etl_includes etl INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(mqtt_etl SYSTEM INTERFACE ${_etl_includes})
endif()

#-------------------------------------------------------------------------------
# The library
#-------------------------------------------------------------------------------
add_library(paho_cpp_static STATIC
    src/packet.cpp
    src/codec.cpp
    src/topic.cpp
    src/utf8.cpp
    src/client.cpp)
add_library(mqtt::client ALIAS paho_cpp_static)

target_include_directories(paho_cpp_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(paho_cpp_static PUBLIC mqtt_etl)
target_compile_features(paho_cpp_static PUBLIC cxx_std_17)

set_target_properties(paho_cpp_static PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME client)

# PIC so a hosted consumer can link this archive into a shared object. Not on a
# bare-metal target: there are no shared objects there, and -fPIC means
# GOT-relative code needing runtime support that nothing will provide. The
# toolchain file cannot switch this off from outside, because a property set
# explicitly on a target beats CMAKE_POSITION_INDEPENDENT_CODE -- so the
# condition has to live here.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set_target_properties(paho_cpp_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# The whole point of the rewrite: it has to build clean without exceptions,
# without RTTI, and without the compiler quietly inserting either back.
#
# The MQTT_REQUIRE_NO_* definitions are checked by a #error in error.hpp against
# the compiler's own __cpp_exceptions / _CPPUNWIND / __cpp_rtti / _CPPRTTI. That
# turns "the flag did not take effect on this toolchain" from a silent broken
# promise into a build failure -- which is how the MSVC gap would have been
# caught the first time.
if(MQTT_NO_EXCEPTIONS)
    target_compile_definitions(paho_cpp_static PUBLIC MQTT_REQUIRE_NO_EXCEPTIONS)
endif()
if(MQTT_NO_RTTI)
    target_compile_definitions(paho_cpp_static PUBLIC MQTT_REQUIRE_NO_RTTI)
endif()

if(NOT MSVC)
    if(MQTT_NO_EXCEPTIONS)
        target_compile_options(paho_cpp_static PUBLIC -fno-exceptions)
    endif()
    if(MQTT_NO_RTTI)
        target_compile_options(paho_cpp_static PUBLIC -fno-rtti)
    endif()

    target_compile_options(paho_cpp_static PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
        -Wnon-virtual-dtor -Wdouble-promotion -Wformat=2)

    if(MQTT_WERROR)
        target_compile_options(paho_cpp_static PRIVATE -Werror)
    endif()

    # One .su file per translation unit, listing each function's frame size and
    # whether it is static, bounded or dynamic. "dynamic" would mean a VLA or
    # alloca -- an unbounded stack demand, which this library must not have --
    # so CI reads these and fails on one. With no recursion anywhere on the
    # protocol path, the sum of all frames is then a hard upper bound on what
    # the library can ask of the stack.
    if(MQTT_STACK_USAGE)
        target_compile_options(paho_cpp_static PRIVATE -fstack-usage)
    endif()
else()
    # /EHsc was already stripped from CMAKE_CXX_FLAGS above; this stops the
    # MSVC standard library from introducing throw sites of its own.
    if(MQTT_NO_EXCEPTIONS)
        target_compile_definitions(paho_cpp_static PUBLIC _HAS_EXCEPTIONS=0)
    endif()
    if(MQTT_NO_RTTI)
        target_compile_options(paho_cpp_static PUBLIC /GR-)
    endif()

    target_compile_options(paho_cpp_static PRIVATE /W4)
    if(MQTT_WERROR)
        target_compile_options(paho_cpp_static PRIVATE /WX)
    endif()
endif()

#-------------------------------------------------------------------------------
# Coverage instrumentation
#
# PUBLIC rather than PRIVATE on purpose. Most of this library is templates that
# live in headers, so the code being measured is compiled into the *test*
# objects, not into libpaho_cpp_static.a. Instrumenting only the library target
# would report high coverage of the four small .cpp files and say nothing at all
# about client.hpp, which is where the state machine actually is.
#
# --coverage has to reach the link line too, hence link_options.
#-------------------------------------------------------------------------------
if(MQTT_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "MQTT_COVERAGE needs a GCC- or Clang-compatible "
                            "toolchain; MSVC has no --coverage equivalent here.")
    endif()
    target_compile_options(paho_cpp_static PUBLIC --coverage -O0 -g)
    target_link_options(paho_cpp_static PUBLIC --coverage)
endif()

#-------------------------------------------------------------------------------
# Tests and examples
#-------------------------------------------------------------------------------
if(MQTT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MQTT_BUILD_EXAMPLES AND UNIX)
    add_subdirectory(examples)
endif()

#-------------------------------------------------------------------------------
# Install
#-------------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS paho_cpp_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# No install(EXPORT) on purpose. ETL is a *public* dependency -- it appears in
# this library's headers -- so an exported package config would have to re-find
# ETL on the consumer's machine, and a config file that gets that wrong is worse
# than none at all. Consume this project with add_subdirectory() or FetchContent
# and link mqtt::client, which carries the ETL include path with it.
