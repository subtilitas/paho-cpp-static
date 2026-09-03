cmake_minimum_required(VERSION 3.16)
project(paho-cpp-static VERSION 0.6.1 LANGUAGES CXX
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
option(MQTT_FUZZ           "Build the libFuzzer targets (clang only)" OFF)

# The prerelease suffix, if any.
#
# project(VERSION ...) accepts digits and dots only, so the "-rc1" of a release
# candidate cannot live there. It lives here instead, and the two are joined
# into MQTT_VERSION_FULL below -- which is what a tag is checked against and
# what names a release archive. Empty for a final release.
set(MQTT_VERSION_PRERELEASE "" CACHE STRING
    "Prerelease suffix, e.g. rc1. Empty for a final release.")

if(MQTT_VERSION_PRERELEASE)
    set(MQTT_VERSION_FULL "${PROJECT_VERSION}-${MQTT_VERSION_PRERELEASE}")
else()
    set(MQTT_VERSION_FULL "${PROJECT_VERSION}")
endif()

set(MQTT_ETL_DIR "" CACHE PATH
    "Path to an Embedded Template Library checkout. If empty, ETL is fetched.")

# The interop job builds a second broker to test against, from an unpacked
# minimosq headers release. Empty means the broker is not built, which is the
# default: nothing in the library or the unit suite depends on it.
set(MQTT_MINIMOSQ_DIR "" CACHE PATH
    "Path to an unpacked minimosq headers release. Enables the interop broker.")

# The dependency pin, in one place. tools/pins.py reads these and checks that
# every workflow cache key agrees, so the version cannot drift between here and
# CI without the build saying so.
set(MQTT_ETL_TAG "20.48.1" CACHE STRING
    "ETL release that MQTT_ETL_COMMIT corresponds to. Documentation for humans.")
set(MQTT_ETL_COMMIT "7f290365a9f1b72bf89b4b77ec7dbe39b228bd8e" CACHE STRING
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
    target_include_directories(mqtt_etl SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${MQTT_ETL_DIR}/include>")
else()
    include(FetchContent)

    # Pinned by commit, not by tag.
    #
    # A tag is a mutable ref. The same name can be moved to different content,
    # and nothing in a build that only records the tag would notice -- which
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
    target_include_directories(mqtt_etl SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${_etl_includes}>")
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

# Not UNIX-only any more: ip_publisher builds on Windows against
# winsock_transport.hpp. examples/CMakeLists.txt decides which demos a platform
# gets.
if(MQTT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

#-------------------------------------------------------------------------------
# Install
#-------------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ETL is a *public* dependency: it appears in this library's headers, so a
# consumer compiles against it whether or not they name it themselves. An
# exported package config therefore has to say where ETL comes from, and a
# config that resolves a public dependency to the wrong copy is worse than no
# config at all.
#
# So it is not guessed at on the consumer's machine. How ETL was satisfied is
# known here, at configure time, and is baked into the generated config:
#
#   fetched by us     -> those exact headers are installed with the library, in
#                        a directory of their own, and the config points at
#                        them. Nothing to find.
#   MQTT_ETL_DIR      -> the consumer already manages ETL, so the config
#                        requires find_package(etl) and says so if it fails.
#
# ETL only installs itself when it is the top-level project, so under
# FetchContent there is no installed ETL for a consumer to find. That is what
# makes the first branch the default rather than an option.
if(MQTT_ETL_DIR)
    set(_mqtt_vendor_etl_default OFF)
else()
    set(_mqtt_vendor_etl_default ON)
endif()

option(MQTT_INSTALL_ETL
       "Install the ETL headers this library was built against alongside it"
       ${_mqtt_vendor_etl_default})

# Its own directory, so a vendored copy cannot collide with an ETL the consumer
# installs into the same prefix.
set(MQTT_ETL_INSTALL_INCLUDEDIR
    "${CMAKE_INSTALL_INCLUDEDIR}/paho-cpp-static/vendor")

if(MQTT_INSTALL_ETL)
    target_include_directories(mqtt_etl SYSTEM INTERFACE
        "$<INSTALL_INTERFACE:${MQTT_ETL_INSTALL_INCLUDEDIR}>")
endif()

set_target_properties(mqtt_etl PROPERTIES EXPORT_NAME etl)

install(TARGETS paho_cpp_static mqtt_etl
        EXPORT paho-cpp-staticTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(MQTT_INSTALL_ETL)
    if(MQTT_ETL_DIR)
        set(_mqtt_etl_header_dir "${MQTT_ETL_DIR}/include/etl")
    else()
        set(_mqtt_etl_header_dir "${etl_SOURCE_DIR}/include/etl")
    endif()

    install(DIRECTORY "${_mqtt_etl_header_dir}"
            DESTINATION "${MQTT_ETL_INSTALL_INCLUDEDIR}")
endif()

install(EXPORT paho-cpp-staticTargets
        NAMESPACE mqtt::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paho-cpp-static)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/paho-cpp-staticConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/paho-cpp-staticConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paho-cpp-static
    PATH_VARS MQTT_ETL_INSTALL_INCLUDEDIR)

# The numeric version only. A release candidate compares equal to the release
# it precedes, because CMake's version comparison has no notion of a suffix --
# so a consumer pinning `1.0.0 EXACT` is satisfied by 1.0.0-rc1. The full
# version, suffix included, is recorded in the config file itself.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/paho-cpp-staticConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/paho-cpp-staticConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/paho-cpp-staticConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/paho-cpp-static)
