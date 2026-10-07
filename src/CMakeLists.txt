# Board support package core: drivers, startup, and syscall stubs.
#
# This is an OBJECT library, not a STATIC library. A static archive drops
# syscalls.c from the link, because the newlib nosys stubs satisfy _write and
# _sbrk first.

add_library(lm3s6965_bsp OBJECT
    startup.c
    system_control.c
    gpio.c
    ethernet.c
    led.c
    switch.c
    trace.c
    uart.c
    ssi.c
    oled.c
    syscalls.c)

target_link_libraries(lm3s6965_bsp PUBLIC lm3s6965_headers)

target_compile_options(lm3s6965_bsp
    PUBLIC ${LM3S6965_ARCH_FLAGS}
    PRIVATE ${LM3S6965_WARNING_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# A consumer that links this target alone gets the CPU flags, the board linker
# script, and the C runtime specs.
target_link_options(lm3s6965_bsp INTERFACE ${LM3S6965_LINK_FLAGS})

# Opt-in archives: neither storage nor protocol belongs in the BSP OBJECT
# library. These targets require no lwIP, startup, syscalls, or linker script.
add_library(lm3s6965_ftp_core STATIC EXCLUDE_FROM_ALL ftp_core.c)
add_library(lm3s6965_ram_file STATIC EXCLUDE_FROM_ALL ram_file.c)
foreach(target IN ITEMS lm3s6965_ftp_core lm3s6965_ram_file)
    target_link_libraries(${target} PUBLIC lm3s6965_ftp_headers)
    target_compile_options(${target} PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
endforeach()

# The caller supplies a lwIP target exporting its headers, lwipopts.h/arch
# paths and configuration definitions. Never import an example's config here.
# Each factory instance compiles the adapter against that caller's stack.
# Runtime remains a single server on a dedicated pinned lwIP 2.2.1 stack.
function(lm3s6965_add_ftp_tcp_library target_name lwip_target)
    if(NOT TARGET "${lwip_target}")
        message(FATAL_ERROR "FTP TCP requires an existing caller-selected lwIP target: ${lwip_target}")
    endif()
    add_library(${target_name} STATIC EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ftp_tcp.c")
    target_link_libraries(${target_name} PUBLIC
        lm3s6965_ftp_core "${lwip_target}")
    target_compile_options(${target_name} PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
endfunction()
