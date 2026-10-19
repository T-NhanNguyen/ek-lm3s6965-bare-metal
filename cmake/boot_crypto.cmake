include_guard(GLOBAL)

# Shared with root builds and standalone verifier consumers. Production root
# definitions/includes follow the existing crypto usage requirements.
include("${CMAKE_CURRENT_LIST_DIR}/boot_trust_root.cmake")

# Consume only the verified local release; no upstream CMake, downloads,
# generated files, source patches, or global warning/configuration changes.
get_filename_component(BOOT_CRYPTO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BOOT_MBEDTLS_DIR "${BOOT_CRYPTO_ROOT}/third_party/mbedtls")
set(BOOT_CRYPTO_MODULES
    asn1parse asn1write bignum bignum_core constant_time
    ecdsa ecp ecp_curves platform_util sha256)
if(NOT EXISTS "${BOOT_MBEDTLS_DIR}/include/mbedtls/build_info.h")
    message(FATAL_ERROR "Missing local Mbed TLS source. Run python3 scripts/setup-dependencies.py manually; see docs/dependencies.md. Configure never fetches dependencies.")
endif()
set(BOOT_CRYPTO_SOURCES "")
foreach(module IN LISTS BOOT_CRYPTO_MODULES)
    list(APPEND BOOT_CRYPTO_SOURCES "${BOOT_MBEDTLS_DIR}/library/${module}.c")
endforeach()
add_library(boot_crypto STATIC ${BOOT_CRYPTO_SOURCES})
target_link_libraries(boot_crypto PUBLIC lm3s6965_boot_trust_root)
target_include_directories(boot_crypto SYSTEM PUBLIC "${BOOT_MBEDTLS_DIR}/include")
target_include_directories(boot_crypto PUBLIC "${BOOT_CRYPTO_ROOT}/config")
target_compile_definitions(boot_crypto PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_boot.h")
target_compile_features(boot_crypto PUBLIC c_std_11)
# Match the boot archive ABI, including standalone verifier consumers.
target_compile_options(boot_crypto PUBLIC -fno-short-enums)
# Upstream warnings are intentionally separate from strict project-owned tests.
target_compile_options(boot_crypto PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_crypto PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
