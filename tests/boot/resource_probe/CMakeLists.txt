cmake_minimum_required(VERSION 3.20)
project(boot_resource_probe LANGUAGES C)
if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Resource probe requires Arm cross compilation, Release (-Os)")
endif()
get_filename_component(ROOT "../../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include(${ROOT}/cmake/boot_crypto.cmake)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
add_compile_options(-mcpu=cortex-m3 -mthumb -mfloat-abi=soft -ffunction-sections -fdata-sections -fno-common -fstack-usage)
target_compile_options(boot_crypto PRIVATE -fstack-usage)
add_executable(resource_probe probe.c console_stubs.c
    ${ROOT}/src/startup.c ${ROOT}/src/syscalls.c
    ${ROOT}/src/boot_verify.c ${ROOT}/src/boot_format.c)
target_include_directories(resource_probe PRIVATE ${ROOT}/include/lm3s6965 ${ROOT}/include)
target_compile_options(resource_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(resource_probe PRIVATE boot_crypto)
# Deliberately use default full newlib, NOT nano/nosys or semihosting specs.
target_link_options(resource_probe PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
    -nostartfiles -T${ROOT}/linker/bootloader.ld
    -Wl,--gc-sections,-z,max-page-size=0x100,-Map=${CMAKE_CURRENT_BINARY_DIR}/resource_probe.map,--cref)

option(RESOURCE_EXECUTION "Build isolated semihosted QEMU measurement fixtures" OFF)
if(RESOURCE_EXECUTION)
    # A renamed, otherwise unchanged generic syscall unit supplies FTP stdio only.
    add_library(resource_ftp_stdio OBJECT ${ROOT}/src/syscalls.c)
    target_compile_definitions(resource_ftp_stdio PRIVATE _sbrk=unused_generic_sbrk)
    target_include_directories(resource_ftp_stdio PRIVATE ${ROOT}/include/lm3s6965)
    # Controlled historical capacity copy; never patch the production script.
    file(READ ${ROOT}/examples/ethernet-ftp/lm3s6965_ftp.ld old_ftp_script)
    string(REPLACE "_heap_start + 8K" "_heap_start + 2K" old_ftp_script "${old_ftp_script}")
    string(REPLACE "<= 8K, \"FTP static" "<= 2K, \"FTP static" old_ftp_script "${old_ftp_script}")
    string(REPLACE "== 8K, \"FTP fixed" "== 2K, \"FTP fixed" old_ftp_script "${old_ftp_script}")
    string(REPLACE "8 KiB total C heap" "2 KiB total C heap" old_ftp_script "${old_ftp_script}")
    string(REPLACE "fixed 8 KiB" "fixed 2 KiB" old_ftp_script "${old_ftp_script}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/old_ftp_2k.ld "${old_ftp_script}")
    foreach(kind IN ITEMS boot ftp old_ftp_2k)
        add_executable(resource_${kind}_execution execution.c console_stubs.c
            ${ROOT}/src/startup.c ${ROOT}/src/boot_verify.c ${ROOT}/src/boot_format.c)
        target_compile_definitions(resource_${kind}_execution PRIVATE LM3S6965_FTP_STACK_MEASUREMENT)
        target_include_directories(resource_${kind}_execution PRIVATE ${ROOT}/include/lm3s6965 ${ROOT}/include)
        target_compile_options(resource_${kind}_execution PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(resource_${kind}_execution PRIVATE boot_crypto)
        if(kind STREQUAL "boot")
            target_sources(resource_${kind}_execution PRIVATE ${ROOT}/src/syscalls.c)
            set(script ${ROOT}/linker/bootloader.ld)
        else()
            target_sources(resource_${kind}_execution PRIVATE ${ROOT}/examples/ethernet-ftp/ftp_sbrk.c
                $<TARGET_OBJECTS:resource_ftp_stdio>)
            set(script ${ROOT}/examples/ethernet-ftp/lm3s6965_ftp.ld)
            if(kind STREQUAL "old_ftp_2k")
                target_compile_definitions(resource_${kind}_execution PRIVATE RESOURCE_OLD_FTP_2K)
                set(script ${CMAKE_CURRENT_BINARY_DIR}/old_ftp_2k.ld)
            endif()
        endif()
        target_link_options(resource_${kind}_execution PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
            -nostartfiles -T${script}
            -Wl,--wrap=calloc,--wrap=free,--wrap=_sbrk
            -Wl,--gc-sections,-z,max-page-size=0x100,-Map=${CMAKE_CURRENT_BINARY_DIR}/resource_${kind}_execution.map,--cref)
    endforeach()
endif()
