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
