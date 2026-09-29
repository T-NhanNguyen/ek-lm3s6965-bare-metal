cmake_minimum_required(VERSION 3.20)

project(lm3s6965_bsp
        VERSION 0.1.0
        DESCRIPTION "Board support package for the TI/Luminary LM3S6965 (ARM Cortex-M3) evaluation board"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LM3S6965_ARCH_FLAGS
    -mcpu=cortex-m3
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -fno-common)

set(LM3S6965_WARNING_FLAGS
    -Wall
    -Wextra
    -Werror)

set(LM3S6965_COMPILE_FLAGS
    ${LM3S6965_ARCH_FLAGS}
    ${LM3S6965_WARNING_FLAGS})

set(LM3S6965_OPTIMIZATION_FLAGS
    $<$<CONFIG:Debug>:-Og>
    $<$<CONFIG:Release>:-Os>)

set(LM3S6965_LINK_FLAGS
    -mcpu=cortex-m3
    -mthumb
    -mfloat-abi=soft
    -T${CMAKE_CURRENT_SOURCE_DIR}/linker/lm3s6965.ld
    -Wl,--gc-sections
    -nostartfiles
    --specs=nano.specs
    --specs=nosys.specs)

function(lm3s6965_add_binary_outputs target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target_name}> ${target_name}.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${target_name}> ${target_name}.hex
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target_name}>
        COMMENT "Generating .bin/.hex and reporting size")
endfunction()

# This repository is the board support package. The BSP targets live in
# include/, src/, and freertos/. The examples consume them.
add_subdirectory(include)
add_subdirectory(src)
add_subdirectory(freertos)
add_subdirectory(examples)
