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
