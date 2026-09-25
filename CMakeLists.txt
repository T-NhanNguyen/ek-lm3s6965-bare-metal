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

set(LM3S6965_COMPILE_FLAGS
    -mcpu=cortex-m3
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -fno-common
    -Wall
    -Wextra
    -Werror)

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
    -Wl,-Map=${PROJECT_NAME}.map
    --specs=nano.specs
    --specs=nosys.specs)

add_executable(${PROJECT_NAME}
    src/startup.c
    src/system_control.c
    src/gpio.c
    src/led.c
    src/trace.c
    src/uart.c
    src/syscalls.c
    src/main.c)

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

target_link_options(${PROJECT_NAME} PRIVATE ${LM3S6965_LINK_FLAGS})

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Generating .bin/.hex and reporting size")
