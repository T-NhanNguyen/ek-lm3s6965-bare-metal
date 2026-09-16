# Cross-compilation toolchain for the TI/Luminary LM3S6965 (ARM Cortex-M3).
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Bare-metal targets have no hosted C runtime, so CMake's default link-based
# compiler probe would fail. Probe by building a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The Homebrew formula `arm-none-eabi-gcc` is NOT usable for bare-metal builds:
# it ships no libc (empty sysroot, no newlib), so its <stdint.h> fails its
# `#include_next <stdint.h>` and the build dies with "stdint.h: No such file
# or directory". Only the official Arm GNU Toolchain is complete. Search the
# official install locations before falling back to PATH.
file(GLOB LM3S6965_DISCOVERED_TOOLCHAIN_ROOTS
    "$ENV{HOME}/.toolchains/arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi"
    "/Applications/ArmGNUToolchain/*/arm-none-eabi"
    "/Applications/ArmGNUToolchain/*"
    "/Applications/ARM")

set(LM3S6965_TOOLCHAIN_BIN_HINTS "")
foreach(toolchain_root IN LISTS LM3S6965_DISCOVERED_TOOLCHAIN_ROOTS)
    list(APPEND LM3S6965_TOOLCHAIN_BIN_HINTS "${toolchain_root}/bin")
endforeach()

set(LM3S6965_TOOLCHAIN_PREFIX "arm-none-eabi-"
    CACHE STRING "GNU Arm Embedded toolchain program prefix")

find_program(LM3S6965_C_COMPILER ${LM3S6965_TOOLCHAIN_PREFIX}gcc
    HINTS ${LM3S6965_TOOLCHAIN_BIN_HINTS}
    REQUIRED)
find_program(LM3S6965_OBJCOPY ${LM3S6965_TOOLCHAIN_PREFIX}objcopy
    HINTS ${LM3S6965_TOOLCHAIN_BIN_HINTS}
    REQUIRED)
find_program(LM3S6965_SIZE ${LM3S6965_TOOLCHAIN_PREFIX}size
    HINTS ${LM3S6965_TOOLCHAIN_BIN_HINTS}
    REQUIRED)
find_program(LM3S6965_GDB ${LM3S6965_TOOLCHAIN_PREFIX}gdb
    HINTS ${LM3S6965_TOOLCHAIN_BIN_HINTS})

# Guard against a libc-less toolchain, with an actionable message.
execute_process(
    COMMAND ${LM3S6965_C_COMPILER} -mcpu=cortex-m3 -mthumb -print-file-name=libc.a
    OUTPUT_VARIABLE LM3S6965_LIBC_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT EXISTS "${LM3S6965_LIBC_PATH}")
    message(FATAL_ERROR
        "The selected toolchain has no C library:\n"
        "  compiler: ${LM3S6965_C_COMPILER}\n"
        "  libc.a  : ${LM3S6965_LIBC_PATH} (missing)\n\n"
        "Install the official Arm GNU Toolchain and re-run:\n"
        "  brew install --cask gcc-arm-embedded\n"
        "or set -DLM3S6965_TOOLCHAIN_BIN_HINTS to a toolchain bin directory.")
endif()

message(STATUS "LM3S6965 toolchain: ${LM3S6965_C_COMPILER}")
message(STATUS "LM3S6965 libc     : ${LM3S6965_LIBC_PATH}")

set(CMAKE_C_COMPILER   ${LM3S6965_C_COMPILER})
set(CMAKE_ASM_COMPILER ${LM3S6965_C_COMPILER})
set(CMAKE_OBJCOPY      ${LM3S6965_OBJCOPY} CACHE FILEPATH "")
set(CMAKE_SIZE         ${LM3S6965_SIZE}    CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
