# Interface target for the LM3S6965 peripheral driver headers.
#
# Consumers include a driver header by bare name (for example, "led.h") instead
# of spelling the lm3s6965/ path, so this target exports include/lm3s6965 itself
# as a usage requirement.

add_library(lm3s6965_headers INTERFACE)

target_include_directories(lm3s6965_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/lm3s6965
    ${CMAKE_CURRENT_SOURCE_DIR}/assets)

# Portable FTP/storage APIs use namespaced includes, without BSP driver paths.
add_library(lm3s6965_ftp_headers INTERFACE)
target_include_directories(lm3s6965_ftp_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
