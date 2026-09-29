# FreeRTOS example. It links the FreeRTOS support target, which exports the
# kernel and the BSP.

add_executable(lm3s6965_freertos_firmware
    main.c)

target_link_libraries(lm3s6965_freertos_firmware PRIVATE lm3s6965_freertos)

target_compile_options(lm3s6965_freertos_firmware PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

target_link_options(lm3s6965_freertos_firmware PRIVATE
    -Wl,-Map=lm3s6965_freertos_firmware.map)

lm3s6965_add_binary_outputs(lm3s6965_freertos_firmware)
