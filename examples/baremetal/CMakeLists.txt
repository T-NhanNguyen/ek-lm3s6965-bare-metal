# Bare-metal example. It links the BSP target and adds only its own main.

add_executable(lm3s6965_firmware
    main.c)

target_link_libraries(lm3s6965_firmware PRIVATE lm3s6965_bsp)

target_compile_options(lm3s6965_firmware PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

target_link_options(lm3s6965_firmware PRIVATE
    -Wl,-Map=lm3s6965_firmware.map)

lm3s6965_add_binary_outputs(lm3s6965_firmware)
