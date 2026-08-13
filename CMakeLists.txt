cmake_minimum_required(VERSION 3.16)
project(paho-cpp-static VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "Statically allocated MQTT 3.1.1 client in C++17")

#-------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------
option(MQTT_BUILD_TESTS    "Build the unit tests"           ON)
option(MQTT_NO_EXCEPTIONS  "Compile with -fno-exceptions"   ON)
option(MQTT_NO_RTTI        "Compile with -fno-rtti"         ON)
option(MQTT_WERROR         "Treat warnings as errors"       OFF)

set(MQTT_ETL_DIR "" CACHE PATH
    "Path to an Embedded Template Library checkout. If empty, ETL is fetched.")

# Treat this as a subproject when someone add_subdirectory()s us.
if(NOT CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(MQTT_BUILD_TESTS OFF)
    set(MQTT_BUILD_EXAMPLES OFF)
endif()

#-------------------------------------------------------------------------------
# Embedded Template Library
#-------------------------------------------------------------------------------
add_library(mqtt_etl INTERFACE)

if(MQTT_ETL_DIR)
    # SYSTEM keeps our -Wconversion/-Wold-style-cast pass from drowning in
    # diagnostics about a third-party header we do not maintain.
    target_include_directories(mqtt_etl SYSTEM INTERFACE "${MQTT_ETL_DIR}/include")
else()
    include(FetchContent)
    FetchContent_Declare(etl
        GIT_REPOSITORY https://github.com/ETLCPP/etl.git
        GIT_TAG        20.39.4
        GIT_SHALLOW    TRUE)
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
    src/client.cpp)
add_library(mqtt::client ALIAS paho_cpp_static)

target_include_directories(paho_cpp_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(paho_cpp_static PUBLIC mqtt_etl)
target_compile_features(paho_cpp_static PUBLIC cxx_std_17)

set_target_properties(paho_cpp_static PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME client)

# The whole point of the rewrite: it has to build clean without exceptions,
# without RTTI, and without the compiler quietly inserting either back.
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
else()
    target_compile_options(paho_cpp_static PRIVATE /W4)
    if(MQTT_WERROR)
        target_compile_options(paho_cpp_static PRIVATE /WX)
    endif()
endif()

#-------------------------------------------------------------------------------
# Tests
#-------------------------------------------------------------------------------
if(MQTT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
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
