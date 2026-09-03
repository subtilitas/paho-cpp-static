# A consumer of the *installed* package, built out of tree against nothing but
# the install prefix.
#
# This is the only way to find out whether install(EXPORT) and the generated
# config actually work. Building inside this repository proves nothing about
# them: add_subdirectory() and FetchContent both bypass the config file
# entirely, so a broken one stays invisible until somebody else hits it.

cmake_minimum_required(VERSION 3.16)
project(paho-cpp-static-consumer LANGUAGES CXX)

find_package(paho-cpp-static REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE mqtt::client)
