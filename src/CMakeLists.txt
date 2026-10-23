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
set_property(TARGET lm3s6965_bsp APPEND PROPERTY INTERFACE_LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../linker/lm3s6965.ld")

# Opt-in archives: neither storage nor protocol belongs in the BSP OBJECT
# library. These targets require no lwIP, startup, syscalls, or linker script.
add_library(lm3s6965_ftp_core STATIC EXCLUDE_FROM_ALL ftp_core.c)
if(LM3S6965_FTP_DIAGNOSTIC)
    target_sources(lm3s6965_ftp_core PRIVATE ftp_diagnostic.c)
    # PUBLIC reaches boot/TCP archives and final slot/platform consumers, not
    # merely the executable. OFF builds have neither record nor module code.
    target_compile_definitions(lm3s6965_ftp_core PUBLIC LM3S6965_FTP_DIAGNOSTIC=1)
endif()
add_library(lm3s6965_ram_file STATIC EXCLUDE_FROM_ALL ram_file.c)
foreach(target IN ITEMS lm3s6965_ftp_core lm3s6965_ram_file)
    target_link_libraries(${target} PUBLIC lm3s6965_ftp_headers)
    target_compile_options(${target} PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
endforeach()

# Ordered diagnostics are opt-in; no production startup/runtime integration.
add_library(lm3s6965_boot_diag_publication STATIC EXCLUDE_FROM_ALL
    boot_diag_publication.c boot_diag.c)
target_include_directories(lm3s6965_boot_diag_publication PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(lm3s6965_boot_diag_publication PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Read-only coherent capture; opt-in only, no startup/protocol integration.
add_library(lm3s6965_boot_snapshot STATIC EXCLUDE_FROM_ALL
    boot_snapshot.c boot_store.c boot_format.c)
target_link_libraries(lm3s6965_boot_snapshot PUBLIC lm3s6965_boot_diag_publication)
target_compile_options(lm3s6965_boot_snapshot PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# HOST service boundary only. No FTP transport or production flash binding.
add_library(lm3s6965_boot_service STATIC EXCLUDE_FROM_ALL
    boot_service.c boot_lifecycle.c)
target_link_libraries(lm3s6965_boot_service PUBLIC lm3s6965_boot_snapshot)
target_compile_options(lm3s6965_boot_service PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Injected controller only: no MMIO binding or startup/runtime integration.
add_library(lm3s6965_boot_flash STATIC EXCLUDE_FROM_ALL
    boot_flash.c boot_store.c boot_format.c)
target_include_directories(lm3s6965_boot_flash PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(lm3s6965_boot_flash PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Separate bounded HOST receive transaction, never a RAM FTP backend.
# Final consumers supply verifier/crypto dependencies; not a linked boot image.
add_library(lm3s6965_boot_receive STATIC EXCLUDE_FROM_ALL
    boot_receive.c boot_lifecycle.c)
target_link_libraries(lm3s6965_boot_receive PUBLIC lm3s6965_boot_flash)
target_compile_options(lm3s6965_boot_receive PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Concrete opt-in FTP boot binding, no enabled updater or startup/MMIO glue.
add_library(lm3s6965_ftp_boot STATIC EXCLUDE_FROM_ALL ftp_boot.c)
target_link_libraries(lm3s6965_ftp_boot PUBLIC
    lm3s6965_ftp_core lm3s6965_boot_service lm3s6965_boot_receive)
target_compile_options(lm3s6965_ftp_boot PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Injected watchdog ownership/progress and clock readiness; no direct MMIO,
# ISR binding or runtime integration.
add_library(lm3s6965_boot_watchdog STATIC EXCLUDE_FROM_ALL
    boot_watchdog.c boot_watchdog_target.c)
target_include_directories(lm3s6965_boot_watchdog PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(lm3s6965_boot_watchdog PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Logical final-handoff intent only; no physical entry/runtime composition.
add_library(lm3s6965_boot_handoff STATIC EXCLUDE_FROM_ALL boot_handoff.c)
target_link_libraries(lm3s6965_boot_handoff PUBLIC lm3s6965_boot_watchdog)
target_compile_options(lm3s6965_boot_handoff PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Candidate/selection decisions only. Consumers must also link the verifier,
# lifecycle, store, format and crypto dependencies; this is not a boot image.
add_library(lm3s6965_boot_launch STATIC EXCLUDE_FROM_ALL boot_launch.c boot_select.c)
target_include_directories(lm3s6965_boot_launch PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(lm3s6965_boot_launch PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# One-shot HOST reset->selection composition, not startup/MMIO or a boot image.
# Consumers supply the verifier/crypto dependencies and final link policy.
add_library(lm3s6965_boot_runtime STATIC EXCLUDE_FROM_ALL
    boot_runtime.c boot_reset_handler.c boot_reset_accounting.c boot_lifecycle.c
    boot_store.c boot_format.c)
target_link_libraries(lm3s6965_boot_runtime PUBLIC lm3s6965_boot_launch)
target_compile_options(lm3s6965_boot_runtime PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Genuine verifier-progress bridge, caller-established watchdog only.
add_library(lm3s6965_boot_runtime_watchdog STATIC EXCLUDE_FROM_ALL
    boot_runtime_watchdog.c)
target_link_libraries(lm3s6965_boot_runtime_watchdog PUBLIC
    lm3s6965_boot_runtime lm3s6965_boot_handoff)
target_compile_options(lm3s6965_boot_runtime_watchdog PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Reusable boundary and setup-only Timer2 producer; no default boot caller or
# updater enable. Final consumers own boot-FLASH closure and entry wiring.
enable_language(ASM)
add_library(lm3s6965_boot_startup STATIC EXCLUDE_FROM_ALL
    boot_startup.c boot_startup_gptm.c boot_startup_producer.c
    boot_startup_target.c boot_startup_terminal.S)
target_link_libraries(lm3s6965_boot_startup PUBLIC
    lm3s6965_boot_handoff lm3s6965_boot_diag_publication)
target_compile_options(lm3s6965_boot_startup PRIVATE ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
# Shared boot-only producer -> protected RUN decision; no physical adapters,
# startup wiring or execution. Consumers still supply verifier/crypto closure.
add_library(lm3s6965_boot_sequence STATIC EXCLUDE_FROM_ALL boot_sequence.c)
target_link_libraries(lm3s6965_boot_sequence PUBLIC
    lm3s6965_boot_startup lm3s6965_boot_runtime_watchdog)
target_compile_options(lm3s6965_boot_sequence PRIVATE ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

# Physical descriptors only; no entry, clock configuration or default executable.
# Final consumers still supply verifier/crypto and prove boot-FLASH closure/fit.
add_library(lm3s6965_boot_target_io STATIC EXCLUDE_FROM_ALL boot_target_io.c)
target_link_libraries(lm3s6965_boot_target_io PUBLIC
    lm3s6965_boot_flash lm3s6965_boot_sequence)
target_compile_options(lm3s6965_boot_target_io PRIVATE ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

add_library(lm3s6965_application_entry OBJECT EXCLUDE_FROM_ALL
    startup.c boot_startup_entry.S)
target_compile_definitions(lm3s6965_application_entry PRIVATE LM3S6965_APPLICATION_CONTINUITY)
target_link_libraries(lm3s6965_application_entry PUBLIC lm3s6965_headers)
target_include_directories(lm3s6965_application_entry PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(lm3s6965_application_entry PRIVATE ${LM3S6965_ARCH_FLAGS})

# Boot enums cross archive/API boundaries and the retained capsule1024 layout
# requires full-width enums. Apply to dependencies themselves, not just their
# callers, and export the same ABI to future consumers. Keep the BSP unchanged.
foreach(target IN ITEMS
    lm3s6965_boot_diag_publication lm3s6965_boot_snapshot
    lm3s6965_boot_service lm3s6965_boot_flash lm3s6965_boot_receive
    lm3s6965_ftp_boot lm3s6965_boot_watchdog lm3s6965_boot_handoff
    lm3s6965_boot_launch lm3s6965_boot_runtime lm3s6965_boot_runtime_watchdog
    lm3s6965_boot_startup lm3s6965_boot_sequence lm3s6965_boot_target_io
    lm3s6965_application_entry)
    target_compile_options(${target} PUBLIC -fno-short-enums)
endforeach()
# Prebuilt libc/libgcc attributes are unchanged: the exact linked runtime helper
# signatures need a separate no-enum-crossing assessment, not warning suppression.

if(LM3S6965_BUILD_BOOT_RESET OR LM3S6965_BUILD_BOOT_DISPATCH_IMAGE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/boot_crypto.cmake")
    add_library(lm3s6965_boot_verifier STATIC boot_verify.c)
    target_link_libraries(lm3s6965_boot_verifier PUBLIC boot_crypto)
    target_include_directories(lm3s6965_boot_verifier PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    target_compile_options(lm3s6965_boot_verifier PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
    # Explicit dependency repairs archive order for select/launch consumers.
    target_link_libraries(lm3s6965_boot_launch PUBLIC lm3s6965_boot_verifier)
    set(boot_image lm3s6965_boot_reset)
    set(boot_image_check_options)
    if(LM3S6965_BUILD_BOOT_DISPATCH_IMAGE)
        set(boot_image lm3s6965_boot_dispatch)
        set(boot_image_check_options --dispatch)
    endif()
    add_executable(${boot_image}
        boot_entry/boot_reset_entry.S boot_entry/boot_vectors.c
        boot_entry/boot_decision_entry.c boot_entry/boot_clock_setup.c
        boot_entry/boot_allocation_support.c)
    if(LM3S6965_BUILD_BOOT_DISPATCH_IMAGE)
        target_sources(${boot_image} PRIVATE boot_handoff_target.c)
        target_compile_definitions(${boot_image} PRIVATE LM3S6965_BOOT_DISPATCH_IMAGE=1)
    endif()
    target_link_libraries(${boot_image} PRIVATE
        lm3s6965_boot_target_io lm3s6965_boot_verifier boot_crypto)
    target_compile_options(${boot_image} PRIVATE
        ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
    if(LM3S6965_BUILD_BOOT_DISPATCH_IMAGE)
        # CMP0138 forwards C flags to the real IPO archive + executable probe.
        # Target link options do not reach it; give only this hosted-main probe
        # the CPU/runtime specs, not the boot entry or application linker script.
        if(NOT POLICY CMP0138)
            message(FATAL_ERROR "Boot dispatch IPO probe requires CMake 3.24 or newer")
        endif()
        cmake_policy(PUSH)
        cmake_policy(SET CMP0138 NEW)
        include(CheckIPOSupported)
        set(boot_dispatch_saved_c_flags "${CMAKE_C_FLAGS}")
        string(JOIN " " boot_dispatch_probe_flags ${LM3S6965_ARCH_FLAGS}
            --specs=nano.specs --specs=nosys.specs)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${boot_dispatch_probe_flags}")
        check_ipo_supported(RESULT boot_dispatch_ipo_supported
            OUTPUT boot_dispatch_ipo_error LANGUAGES C)
        set(CMAKE_C_FLAGS "${boot_dispatch_saved_c_flags}")
        unset(boot_dispatch_saved_c_flags)
        unset(boot_dispatch_probe_flags)
        cmake_policy(POP)
        if(NOT boot_dispatch_ipo_supported)
            message(FATAL_ERROR "Boot dispatch requires IPO support: ${boot_dispatch_ipo_error}")
        endif()
        # Dispatch-only Release policy, including every reachable source archive.
        # Leave the decision configuration and shared crypto configuration intact.
        set_property(TARGET ${boot_image}
            lm3s6965_boot_target_io lm3s6965_boot_verifier boot_crypto
            lm3s6965_boot_flash lm3s6965_boot_sequence lm3s6965_boot_startup
            lm3s6965_boot_diag_publication lm3s6965_boot_runtime_watchdog
            lm3s6965_boot_handoff lm3s6965_boot_watchdog lm3s6965_boot_runtime
            lm3s6965_boot_launch
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        # The LTO link needs the size policy too, after the default Release -O3.
        target_link_options(${boot_image} PRIVATE $<$<CONFIG:Release>:-Os>)
        # Keep real public definitions required by the unchanged image/static
        # oracles. These roots do not prohibit inlining or introduce dummy code.
        foreach(symbol IN ITEMS
            boot_sequence_launch boot_verify_stored _sbrk
            boot_runtime_watchdog_launch
            boot_handoff_target_init boot_handoff_target_observe
            boot_handoff_target_operation boot_handoff_target_terminal_intent
            boot_startup_prepare boot_startup_begin_dispatch
            boot_startup_dispatch boot_startup_stop
            boot_startup_gptm_read boot_startup_gptm_claim
            boot_startup_gptm_direct_read boot_startup_gptm_direct_write
            boot_startup_gptm_direct_delay)
            target_link_options(${boot_image} PRIVATE
                "$<$<CONFIG:Release>:-Wl,--undefined=${symbol}>")
        endforeach()
    endif()
    # Deliberately NOT LM3S6965_LINK_FLAGS: that carries the application script.
    target_link_options(${boot_image} PRIVATE
        -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -nostartfiles
        --specs=nano.specs --specs=nosys.specs
        "-T${CMAKE_CURRENT_SOURCE_DIR}/../linker/bootloader.ld"
        -Wl,--entry=Boot_Reset_Handler -Wl,--gc-sections
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${boot_image}.map")
    set_property(TARGET ${boot_image} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../linker/bootloader.ld")
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET ${boot_image} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-boot-reset-image.py"
            "$<TARGET_FILE:${boot_image}>" ${boot_image_check_options}
        COMMENT "Checking exact boot vectors, reset entry and load closure")
    lm3s6965_add_binary_outputs(${boot_image})
endif()

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
