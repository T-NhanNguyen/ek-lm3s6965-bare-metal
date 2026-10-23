cmake_minimum_required(VERSION 3.20)
project(foreground_owner_regressions LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(FOREGROUND_SANITIZERS "Native ASan/UBSan" ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(NOT EXISTS "${ROOT}/third_party/lwip/src/include/lwip/init.h")
    message(FATAL_ERROR "Offline pinned lwIP checkout required; no download")
endif()
add_library(foreground_lwip STATIC
    "${ROOT}/third_party/lwip/src/core/altcp.c"
    "${ROOT}/third_party/lwip/src/core/altcp_alloc.c"
    "${ROOT}/third_party/lwip/src/core/altcp_tcp.c"
    "${ROOT}/third_party/lwip/src/core/def.c"
    "${ROOT}/third_party/lwip/src/core/dns.c"
    "${ROOT}/third_party/lwip/src/core/inet_chksum.c"
    "${ROOT}/third_party/lwip/src/core/init.c"
    "${ROOT}/third_party/lwip/src/core/ip.c"
    "${ROOT}/third_party/lwip/src/core/mem.c"
    "${ROOT}/third_party/lwip/src/core/memp.c"
    "${ROOT}/third_party/lwip/src/core/netif.c"
    "${ROOT}/third_party/lwip/src/core/pbuf.c"
    "${ROOT}/third_party/lwip/src/core/raw.c"
    "${ROOT}/third_party/lwip/src/core/stats.c"
    "${ROOT}/third_party/lwip/src/core/sys.c"
    "${ROOT}/third_party/lwip/src/core/tcp.c"
    "${ROOT}/third_party/lwip/src/core/tcp_in.c"
    "${ROOT}/third_party/lwip/src/core/tcp_out.c"
    "${ROOT}/third_party/lwip/src/core/timeouts.c"
    "${ROOT}/third_party/lwip/src/core/udp.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/acd.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/autoip.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/dhcp.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/etharp.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/icmp.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/igmp.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/ip4.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/ip4_addr.c"
    "${ROOT}/third_party/lwip/src/core/ipv4/ip4_frag.c"
    "${ROOT}/third_party/lwip/src/netif/ethernet.c"
)
target_include_directories(foreground_lwip PUBLIC
    "${ROOT}/tests/ethernet/platform/shims"
    "${ROOT}/examples/ethernet-ftp" "${ROOT}/third_party/lwip/src/include"
    "${ROOT}/include" "${ROOT}/include/lm3s6965")
target_compile_definitions(foreground_lwip PUBLIC LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1)
add_library(foreground_continuity STATIC
    "${ROOT}/src/boot_startup.c" "${ROOT}/src/boot_startup_gptm.c" "${ROOT}/src/boot_watchdog.c"
    "${ROOT}/src/boot_diag_publication.c" "${ROOT}/src/boot_diag.c")
target_include_directories(foreground_continuity PUBLIC "${ROOT}/include")
# Owner assertions stay on the HOST admission fixture; application tick only.
add_executable(foreground_owner owner_test.c "${ROOT}/examples/ethernet-ftp/net_clock.c")
target_link_libraries(foreground_owner PRIVATE foreground_continuity)
target_compile_definitions(foreground_owner PRIVATE LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1)
target_include_directories(foreground_owner PRIVATE
    "${ROOT}/examples/ethernet-ftp" "${ROOT}/third_party/lwip/src/include"
    "${ROOT}/tests/ethernet/platform/shims" "${ROOT}/include/lm3s6965")
# Strong fixture port symbols, never the non-ARM production port.
add_executable(foreground_clock clock_switch_test.c "${ROOT}/examples/ethernet-ftp/net_clock.c")
target_compile_definitions(foreground_clock PRIVATE LM3S6965_FTP_FOREGROUND_OWNER=1
    LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1)
target_link_libraries(foreground_clock PRIVATE foreground_continuity)
target_include_directories(foreground_clock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/clock_shims"
    "${ROOT}/tests/ethernet/platform/shims" "${ROOT}/third_party/lwip/src/include"
    "${ROOT}/examples/ethernet-ftp" "${ROOT}/include/lm3s6965")
# Reuse the existing HOST assertions, without configuring unrelated boot suites.
add_executable(foreground_boot_startup "${ROOT}/tests/boot/test_startup.c")
target_link_libraries(foreground_boot_startup PRIVATE foreground_continuity)
add_executable(foreground_netif netif_test.c)
target_link_libraries(foreground_netif PRIVATE foreground_lwip)
add_executable(foreground_tcp tcp_service_test.c "${ROOT}/src/ftp_core.c" "${ROOT}/src/ram_file.c")
target_include_directories(foreground_tcp PRIVATE
    "${ROOT}/tests/ethernet/platform/shims" "${ROOT}/examples/ethernet-ftp"
    "${ROOT}/third_party/lwip/src/include" "${ROOT}/include")
target_compile_definitions(foreground_tcp PRIVATE LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1)
foreach(target IN ITEMS foreground_owner foreground_clock foreground_netif foreground_tcp
                        foreground_lwip foreground_continuity foreground_boot_startup)
    target_compile_options(${target} PRIVATE -Wall -Wextra -UNDEBUG)
    if(FOREGROUND_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
endforeach()
enable_testing()
foreach(group IN ITEMS owner clock netif tcp target)
    add_test(NAME foreground_${group} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runner.py" --bin-dir "${CMAKE_CURRENT_BINARY_DIR}"
        --group ${group} --all)
    set_tests_properties(foreground_${group} PROPERTIES ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;ASAN_OPTIONS=halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endforeach()
