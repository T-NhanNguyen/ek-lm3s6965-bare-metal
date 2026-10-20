option(LM3S6965_FTP_STACK_MEASUREMENT
    "Paint the FTP reset stack and emit scoped compiler stack-usage reports" OFF)

# Do not include upstream Filelists.cmake: it writes generated files into the
# submodule. Compile only core/IPv4/Ethernet with this target's NO_SYS options.
set(LWIP_DIR "${PROJECT_SOURCE_DIR}/third_party/lwip")
if(NOT EXISTS "${LWIP_DIR}/src/include/lwip/init.h")
    message(FATAL_ERROR "Ethernet FTP requires the already populated lwIP 2.2.1 submodule (no automatic fetch)")
endif()
file(GLOB ftp_lwip_sources CONFIGURE_DEPENDS
    "${LWIP_DIR}/src/core/*.c" "${LWIP_DIR}/src/core/ipv4/*.c")
add_library(lm3s6965_ftp_lwip OBJECT ${ftp_lwip_sources}
    "${LWIP_DIR}/src/netif/ethernet.c")
target_include_directories(lm3s6965_ftp_lwip PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${LWIP_DIR}/src/include")
target_compile_options(lm3s6965_ftp_lwip PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Select BSP sources, NOT lm3s6965_bsp: its link interface and syscalls retain
# the old linker and unbounded-for-FTP console/heap contracts.
add_library(lm3s6965_ftp_platform OBJECT
    main.c net_clock.c lm3s_netif.c ftp_sbrk.c
    "${PROJECT_SOURCE_DIR}/src/startup.c"
    "${PROJECT_SOURCE_DIR}/src/system_control.c"
    "${PROJECT_SOURCE_DIR}/src/gpio.c"
    "${PROJECT_SOURCE_DIR}/src/ethernet.c")
target_link_libraries(lm3s6965_ftp_platform PUBLIC
    lm3s6965_headers lm3s6965_ftp_lwip)
target_compile_options(lm3s6965_ftp_platform PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

lm3s6965_add_ftp_tcp_library(lm3s6965_ftp_tcp lm3s6965_ftp_lwip)
# main.c binds caller-owned RAM storage before transport startup.
target_link_libraries(lm3s6965_ftp_platform PRIVATE
    lm3s6965_ftp_headers)

add_executable(lm3s6965_ethernet_ftp)
target_link_libraries(lm3s6965_ethernet_ftp PRIVATE
    lm3s6965_ftp_platform lm3s6965_ftp_lwip
    lm3s6965_ftp_tcp lm3s6965_ram_file)
target_link_options(lm3s6965_ethernet_ftp PRIVATE
    ${LM3S6965_ARCH_FLAGS}
    "-T${CMAKE_CURRENT_SOURCE_DIR}/lm3s6965_ftp.ld"
    -Wl,--gc-sections -Wl,-Map=lm3s6965_ethernet_ftp.map
    -nostartfiles --specs=nano.specs --specs=nosys.specs)
if(LM3S6965_FTP_STACK_MEASUREMENT)
    # The BSP's independent startup compilation and all unrelated targets stay
    # unchanged. No optimization override: Release keeps the existing -Os.
    target_compile_definitions(lm3s6965_ftp_platform PRIVATE
        LM3S6965_FTP_STACK_MEASUREMENT=1)
    foreach(target IN ITEMS lm3s6965_ftp_platform lm3s6965_ftp_lwip
            lm3s6965_ftp_tcp lm3s6965_ftp_core lm3s6965_ram_file)
        target_compile_options(${target} PRIVATE -fstack-usage)
    endforeach()
endif()
lm3s6965_add_binary_outputs(lm3s6965_ethernet_ftp)

set(LM3S6965_FTP_FOREGROUND_SLOT "A" CACHE STRING
    "Authenticated application slot for the foreground-owner image (A or B)")
set_property(CACHE LM3S6965_FTP_FOREGROUND_SLOT PROPERTY STRINGS A B)
if(LM3S6965_FTP_FOREGROUND_OWNER)
    if(NOT LM3S6965_FTP_FOREGROUND_SLOT MATCHES "^[AB]$")
        message(FATAL_ERROR "LM3S6965_FTP_FOREGROUND_SLOT must be A or B")
    endif()
    string(TOLOWER "${LM3S6965_FTP_FOREGROUND_SLOT}" foreground_slot)
    if(LM3S6965_FTP_FOREGROUND_SLOT STREQUAL "A")
        set(foreground_pattern 1) # 2 s breathing pulse density
    else()
        set(foreground_pattern 2) # 100/100/100/500 ms double blink
    endif()
    # Separate compilation leaves standalone startup/clock policy unchanged.
    # No BSP link interface, duplicate cold reset startup or system PLL owner.
    # system_control.c supplies the driver's peripheral-clock enable only;
    # foreground main never calls PLL setup.
    add_library(lm3s6965_ftp_foreground_platform OBJECT
        main.c net_clock.c net_clock_port.c lm3s_netif.c foreground_owner.c ftp_sbrk.c
        fixture_led.c fixture_pattern.c
        "${PROJECT_SOURCE_DIR}/src/led.c"
        "${PROJECT_SOURCE_DIR}/src/system_control.c"
        "${PROJECT_SOURCE_DIR}/src/gpio.c"
        "${PROJECT_SOURCE_DIR}/src/ethernet.c")
    target_compile_definitions(lm3s6965_ftp_foreground_platform PRIVATE
        LM3S6965_FTP_FOREGROUND_OWNER=1
        LM3S6965_FTP_FIXTURE_PATTERN=${foreground_pattern})
    target_link_libraries(lm3s6965_ftp_foreground_platform PUBLIC
        lm3s6965_headers lm3s6965_ftp_headers lm3s6965_ftp_lwip
        lm3s6965_boot_startup)
    target_compile_options(lm3s6965_ftp_foreground_platform PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
    add_executable(lm3s6965_ethernet_ftp_foreground)
    target_link_libraries(lm3s6965_ethernet_ftp_foreground PRIVATE
        lm3s6965_ftp_foreground_platform lm3s6965_application_entry
        lm3s6965_ftp_lwip lm3s6965_ftp_tcp lm3s6965_ram_file)
    # Existing slot linker reserves the original continuity/diagnostic objects
    # and bounds _sbrk below them. Unlike standalone's fixed 8 KiB heap, this
    # opt-in slot uses remaining lower SRAM. No production fit is implied.
    set(foreground_linker "${PROJECT_SOURCE_DIR}/linker/app_${foreground_slot}.ld")
    # Align the ELF entry with the authenticated application reset vector.
    target_link_options(lm3s6965_ethernet_ftp_foreground PRIVATE
        ${LM3S6965_ARCH_FLAGS} "-T${foreground_linker}"
        -Wl,--entry=Application_Reset_Handler
        -Wl,-z,max-page-size=0x100 -Wl,--gc-sections
        -Wl,-Map=lm3s6965_ethernet_ftp_foreground.map
        -nostartfiles --specs=nano.specs --specs=nosys.specs)
    set_property(TARGET lm3s6965_ethernet_ftp_foreground APPEND PROPERTY
        LINK_DEPENDS "${foreground_linker}")
    lm3s6965_add_binary_outputs(lm3s6965_ethernet_ftp_foreground)
    # This is an application payload only. A real authenticated dispatch and
    # boot-flash/retained-lifetime IO + clock callbacks are external requirements;
    # no simulated clock/progress, protected boot image or ftp_boot is linked.
endif()
