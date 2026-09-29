# FreeRTOS support for the LM3S6965 (ARM Cortex-M3).
#
# The kernel source is the upstream FreeRTOS-Kernel submodule at
# third_party/FreeRTOS-Kernel. The kernel CMake creates the freertos_kernel
# static library and the freertos_kernel_port object library.

set(FREERTOS_PORT GCC_ARM_CM3 CACHE STRING "FreeRTOS kernel port name")
set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")

# The kernel expects an interface target named freertos_config that supplies the
# directory holding FreeRTOSConfig.h. That config includes the driver header
# system_control.h, so the driver header interface is linked in as well.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(freertos_config INTERFACE lm3s6965_headers)

add_subdirectory(
    ${PROJECT_SOURCE_DIR}/third_party/FreeRTOS-Kernel
    ${CMAKE_BINARY_DIR}/FreeRTOS-Kernel)

# The upstream kernel does not set the target CPU flags. Apply the architecture
# flags to the kernel and port so the Cortex-M3 code and inline assembly build.
target_compile_options(freertos_kernel PRIVATE
    ${LM3S6965_ARCH_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})
target_compile_options(freertos_kernel_port PRIVATE
    ${LM3S6965_ARCH_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})

# Board default hooks. A consumer that supplies its own hooks links
# freertos_kernel and lm3s6965_bsp directly instead.
add_library(lm3s6965_freertos OBJECT hooks.c)
target_link_libraries(lm3s6965_freertos PUBLIC freertos_kernel lm3s6965_bsp)
# CMake does not propagate an object library's objects through another object
# library, so the BSP objects are attached explicitly. This keeps
# lm3s6965_freertos the single link target for a FreeRTOS consumer.
target_sources(lm3s6965_freertos INTERFACE $<TARGET_OBJECTS:lm3s6965_bsp>)
target_compile_options(lm3s6965_freertos PRIVATE
    ${LM3S6965_COMPILE_FLAGS}
    ${LM3S6965_OPTIMIZATION_FLAGS})
