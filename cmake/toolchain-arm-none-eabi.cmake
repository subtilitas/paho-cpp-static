# Bare-metal ARM cross-compilation, for measuring what this library actually
# costs on the class of hardware it was written for.
#
#   cmake -S . -B build-m4 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake \
#         -DMQTT_TARGET_CPU=cortex-m4 \
#         -DMQTT_BUILD_TESTS=OFF -DMQTT_BUILD_EXAMPLES=OFF
#   cmake --build build-m4
#
# This builds the static library and nothing else. There is no linker script,
# no startup code and no runtime here, because none of that is this project's
# business -- the deliverable is `libpaho_cpp_static.a` plus headers, and the
# question this file answers is "does it compile clean for the target, and how
# big is it when it does".
#
# CMAKE_TRY_COMPILE_TARGET_TYPE is the load-bearing line. CMake's compiler
# check links a full executable by default, which cannot succeed for a bare
# target without a linker script -- so the toolchain would be rejected before
# a single source file was seen. Telling CMake to check with a static library
# instead is the documented way out, and it is exactly what we are building.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MQTT_TARGET_CPU "cortex-m4" CACHE STRING
    "-mcpu value for the cross build (e.g. cortex-m0plus, cortex-m4, cortex-m33)")

set(MQTT_TOOLCHAIN_PREFIX "arm-none-eabi-" CACHE STRING
    "Cross toolchain prefix.")

set(CMAKE_C_COMPILER   "${MQTT_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${MQTT_TOOLCHAIN_PREFIX}g++")
set(CMAKE_AR           "${MQTT_TOOLCHAIN_PREFIX}ar")
set(CMAKE_RANLIB       "${MQTT_TOOLCHAIN_PREFIX}ranlib")
set(CMAKE_OBJCOPY      "${MQTT_TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_SIZE         "${MQTT_TOOLCHAIN_PREFIX}size")

# Soft float throughout. The library does no floating-point arithmetic at all,
# so there is nothing to gain from an FPU ABI here -- and picking one would
# make this archive incompatible with an application that chose the other.
# Soft-float objects link against both.
set(_mqtt_arch_flags "-mcpu=${MQTT_TARGET_CPU} -mthumb")

# -ffunction-sections/-fdata-sections so the consuming application's
# --gc-sections can drop whatever it never calls. This is also how the
# footprint numbers are measured, so building any other way would report a
# figure nobody can reproduce.
set(_mqtt_size_flags "-ffunction-sections -fdata-sections")

set(CMAKE_C_FLAGS_INIT   "${_mqtt_arch_flags} ${_mqtt_size_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mqtt_arch_flags} ${_mqtt_size_flags}")

# Nothing on the host is a valid dependency for a bare-metal target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
