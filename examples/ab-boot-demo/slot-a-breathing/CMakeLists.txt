# Slot-A payload only: deliberately no BSP OBJECT library, cold startup,
# default linker, crypto, network, or boot reset/dispatch implementation.
add_executable(lm3s6965_boot_breathing_led
    main.c
    "${PROJECT_SOURCE_DIR}/src/led.c"
    "${PROJECT_SOURCE_DIR}/src/system_control.c")
target_link_libraries(lm3s6965_boot_breathing_led PRIVATE
    lm3s6965_headers lm3s6965_application_entry lm3s6965_boot_startup)
target_compile_options(lm3s6965_boot_breathing_led PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS} -fno-short-enums)
set(breathing_linker "${PROJECT_SOURCE_DIR}/linker/app_a.ld")
target_link_options(lm3s6965_boot_breathing_led PRIVATE
    ${LM3S6965_ARCH_FLAGS} "-T${breathing_linker}"
    -Wl,--entry=Application_Reset_Handler
    -Wl,-z,max-page-size=0x100 -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_boot_breathing_led.map"
    -nostartfiles --specs=nano.specs --specs=nosys.specs)
set_property(TARGET lm3s6965_boot_breathing_led APPEND PROPERTY
    LINK_DEPENDS "${breathing_linker}")
# Fixture-local filtering: even empty SRAM .data can inflate unfiltered objcopy.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(breathing_image_generator "${CMAKE_CURRENT_SOURCE_DIR}/generate-slot-a-images.py")
set_property(TARGET lm3s6965_boot_breathing_led APPEND PROPERTY LINK_DEPENDS
    "${breathing_image_generator}" "${PROJECT_SOURCE_DIR}/tools/boot/package.py")

add_custom_command(TARGET lm3s6965_boot_breathing_led POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${breathing_image_generator}"
        --objcopy "${CMAKE_OBJCOPY}"
        --elf "$<TARGET_FILE:lm3s6965_boot_breathing_led>"
        --bin "${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_boot_breathing_led.bin"
        --hex "${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_boot_breathing_led.hex"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:lm3s6965_boot_breathing_led>"
    COMMENT "Generating filtered slot-A .bin/.hex and reporting size"
    VERBATIM)

# Keep the LED-only fixture independent of production trust and network code.
if(NOT LM3S6965_BUILD_AB_BOOT_DEMO_FTP)
    return()
endif()

# Production foreground FTP receiver, with the breathing pattern and peer B.
# The verifier/crypto closure is mandatory; slot payload fit is not implied.
if(NOT LM3S6965_BOOT_PHYSICAL_PRODUCTION OR NOT TARGET lm3s6965_boot_verifier)
    message(FATAL_ERROR "Slot-A FTP image requires the verifier/crypto closure: configure with LM3S6965_BUILD_BOOT_RESET=ON (or DISPATCH) plus LM3S6965_BOOT_PHYSICAL_PRODUCTION=ON and an explicit PUBLIC key")
endif()
set(ab_ftp_eth_dir "${PROJECT_SOURCE_DIR}/examples/ethernet-ftp")
set(ab_ftp_lwip_dir "${PROJECT_SOURCE_DIR}/third_party/lwip")
if(NOT EXISTS "${ab_ftp_lwip_dir}/src/include/lwip/init.h")
    message(FATAL_ERROR "Slot-A FTP image requires the already populated lwIP 2.2.1 submodule (no automatic fetch)")
endif()
file(GLOB ab_ftp_lwip_sources_a CONFIGURE_DEPENDS
    "${ab_ftp_lwip_dir}/src/core/*.c" "${ab_ftp_lwip_dir}/src/core/ipv4/*.c")
add_library(lm3s6965_ab_lwip_a OBJECT ${ab_ftp_lwip_sources_a}
    "${ab_ftp_lwip_dir}/src/netif/ethernet.c")
target_include_directories(lm3s6965_ab_lwip_a PUBLIC
    "${ab_ftp_eth_dir}" "${ab_ftp_lwip_dir}/src/include")
target_compile_options(lm3s6965_ab_lwip_a PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})

add_library(lm3s6965_ab_ftp_platform_a OBJECT
    "${ab_ftp_eth_dir}/main.c"
    "${ab_ftp_eth_dir}/net_clock.c"
    "${ab_ftp_eth_dir}/net_clock_port.c"
    "${ab_ftp_eth_dir}/lm3s_netif.c"
    "${ab_ftp_eth_dir}/foreground_owner.c"
    "${ab_ftp_eth_dir}/ftp_sbrk.c"
    "${ab_ftp_eth_dir}/fixture_led.c"
    "${ab_ftp_eth_dir}/fixture_pattern.c"
    "${ab_ftp_eth_dir}/ftp_firmware_receive.c"
    "${PROJECT_SOURCE_DIR}/examples/ab-boot-demo/ftp_production.c"
    "${PROJECT_SOURCE_DIR}/src/led.c"
    "${PROJECT_SOURCE_DIR}/src/system_control.c"
    "${PROJECT_SOURCE_DIR}/src/gpio.c"
    "${PROJECT_SOURCE_DIR}/src/ethernet.c")
target_compile_definitions(lm3s6965_ab_ftp_platform_a PRIVATE
    LM3S6965_FTP_FOREGROUND_OWNER=1
    LM3S6965_FTP_FIRMWARE_RECEIVE=1
    LM3S6965_FTP_FIXTURE_PATTERN=1
    LM3S6965_AB_FTP_SLOT=0)
target_include_directories(lm3s6965_ab_ftp_platform_a PRIVATE
    "${ab_ftp_eth_dir}")
target_link_libraries(lm3s6965_ab_ftp_platform_a PUBLIC
    lm3s6965_headers lm3s6965_ftp_headers lm3s6965_ab_lwip_a
    lm3s6965_boot_startup)
target_link_libraries(lm3s6965_ab_ftp_platform_a PRIVATE
    lm3s6965_ftp_boot)
# Boot enums cross archive/API boundaries (production config builds boot
# configs); keep the full-width ABI the boot archives themselves require.
target_compile_options(lm3s6965_ab_ftp_platform_a PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS} -fno-short-enums)

lm3s6965_add_ftp_tcp_library(lm3s6965_ab_ftp_tcp_a lm3s6965_ab_lwip_a)

add_executable(lm3s6965_ab_ftp_a)
target_link_libraries(lm3s6965_ab_ftp_a PRIVATE
    lm3s6965_ab_ftp_platform_a lm3s6965_application_entry
    lm3s6965_ab_lwip_a lm3s6965_ab_ftp_tcp_a lm3s6965_ram_file
    lm3s6965_ftp_boot lm3s6965_boot_target_io lm3s6965_boot_startup
    lm3s6965_boot_verifier boot_crypto)
set(ab_ftp_linker_a "${PROJECT_SOURCE_DIR}/linker/app_a.ld")
target_link_options(lm3s6965_ab_ftp_a PRIVATE
    ${LM3S6965_ARCH_FLAGS} "-T${ab_ftp_linker_a}"
    -Wl,--entry=Application_Reset_Handler
    -Wl,-z,max-page-size=0x100 -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_ab_ftp_a.map"
    -nostartfiles --specs=nano.specs --specs=nosys.specs)
set_property(TARGET lm3s6965_ab_ftp_a APPEND PROPERTY
    LINK_DEPENDS "${ab_ftp_linker_a}" "${breathing_image_generator}"
    "${PROJECT_SOURCE_DIR}/tools/boot/package.py")
add_custom_command(TARGET lm3s6965_ab_ftp_a POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "${Python3_EXECUTABLE}" "${breathing_image_generator}"
        --objcopy "${CMAKE_OBJCOPY}"
        --elf "$<TARGET_FILE:lm3s6965_ab_ftp_a>"
        --bin "${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_ab_ftp_a.bin"
        --hex "${CMAKE_CURRENT_BINARY_DIR}/lm3s6965_ab_ftp_a.hex"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:lm3s6965_ab_ftp_a>"
    COMMENT "Generating filtered slot-A FTP .bin/.hex and reporting size"
    VERBATIM)
