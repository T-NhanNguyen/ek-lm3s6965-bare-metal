# Separate raw diagnostic; never replaces the link-only example.
add_executable(lm3s6965_ethernet_raw main.c raw_protocol.c)
target_link_libraries(lm3s6965_ethernet_raw PRIVATE lm3s6965_bsp)
target_compile_options(lm3s6965_ethernet_raw PRIVATE
    ${LM3S6965_COMPILE_FLAGS} ${LM3S6965_OPTIMIZATION_FLAGS})
target_link_options(lm3s6965_ethernet_raw PRIVATE
    -Wl,-Map=lm3s6965_ethernet_raw.map)
lm3s6965_add_binary_outputs(lm3s6965_ethernet_raw)
