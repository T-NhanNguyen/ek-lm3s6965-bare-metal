cmake_minimum_required(VERSION 3.20)

project(lm3s6965_firmware
        VERSION 0.1.0
        DESCRIPTION "Bare-metal firmware for the TI/Luminary LM3S6965 (ARM Cortex-M3)"
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

# Sources shared by the bare-metal and FreeRTOS targets.
set(LM3S6965_DRIVER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/startup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/system_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/switch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/syscalls.c)

set(FREERTOS_PROJECT_NAME lm3s6965_freertos_firmware)

# The driver headers are exported by an interface target so every consumer
# includes them by bare name (for example, "led.h") with no lm3s6965/ path.
add_subdirectory(include)

add_executable(${PROJECT_NAME}
    ${LM3S6965_DRIVER_SOURCES}
    src/main.c)

target_link_libraries(${PROJECT_NAME} PRIVATE lm3s6965_headers)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

target_link_options(${PROJECT_NAME} PRIVATE
    ${LM3S6965_LINK_FLAGS}
    -Wl,-Map=${PROJECT_NAME}.map)

lm3s6965_add_binary_outputs(${PROJECT_NAME})

add_subdirectory(freertos)
