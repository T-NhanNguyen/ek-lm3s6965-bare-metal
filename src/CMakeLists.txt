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
