# PHY link diagnostic. Use the same BSP and link settings as baremetal.
add_executable(lm3s6965_ethernet_link
    main.c)

target_link_libraries(lm3s6965_ethernet_link PRIVATE lm3s6965_bsp)

target_compile_options(lm3s6965_ethernet_link PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

target_link_options(lm3s6965_ethernet_link PRIVATE
    -Wl,-Map=lm3s6965_ethernet_link.map)

lm3s6965_add_binary_outputs(lm3s6965_ethernet_link)
