include_guard(GLOBAL)

option(LM3S6965_BOOT_PHYSICAL_PRODUCTION
    "Select an explicitly supplied production PUBLIC boot root (does not enable a boot executable)" OFF)
set(LM3S6965_BOOT_PUBLIC_KEY_FILE "" CACHE FILEPATH
    "Explicit raw 65-byte SEC1 uncompressed P-256 PUBLIC key; never a private key")

add_library(lm3s6965_boot_trust_root INTERFACE)
if(LM3S6965_BOOT_PHYSICAL_PRODUCTION)
    # Also guard direct source consumers in this directory tree: omitting the
    # root target must fail compilation, never silently select demo trust.
    add_compile_definitions(LM3S6965_BOOT_PHYSICAL_PRODUCTION=1)
    set(_boot_root_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/boot-public-root")
    set(_boot_root_header "${_boot_root_dir}/lm3s6965_boot_production_public_key.h")
    # Permit only the ignored source/build subtree or an external build tree.
    # Compare canonical path components so lookalikes and symlink escapes into
    # other source directories cannot place generated PUBLIC material there.
    file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." _boot_source)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" _boot_binary)
    file(RELATIVE_PATH _boot_relative "${_boot_source}" "${_boot_binary}")
    if(NOT IS_ABSOLUTE "${_boot_relative}"
            AND NOT _boot_relative MATCHES "^\\.\\.(/|$)"
            AND NOT _boot_relative MATCHES "^build(/|$)")
        message(FATAL_ERROR "Production PUBLIC root requires an external build directory or the source/build subtree")
    endif()
    # Reject unsafe destinations before touching any previously generated root.
    # In valid output trees, remove stale trust even when key validation fails.
    file(REMOVE "${_boot_root_header}")
    if(NOT LM3S6965_BOOT_PUBLIC_KEY_FILE)
        message(FATAL_ERROR "Production requires explicit LM3S6965_BOOT_PUBLIC_KEY_FILE: exactly 65 raw SEC1 uncompressed P-256 PUBLIC bytes; no demo fallback")
    endif()
    get_filename_component(_boot_public_file "${LM3S6965_BOOT_PUBLIC_KEY_FILE}"
        ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_boot_public_file}" OR IS_DIRECTORY "${_boot_public_file}")
        message(FATAL_ERROR "Production PUBLIC key file is missing or not a regular file: ${_boot_public_file}")
    endif()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(_boot_validator "${CMAKE_CURRENT_LIST_DIR}/../scripts/validate-boot-public-key.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_boot_public_file}" "${_boot_validator}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${_boot_validator}"
        --public-key "${_boot_public_file}" --header "${_boot_root_header}"
        RESULT_VARIABLE _boot_key_result OUTPUT_VARIABLE _boot_fingerprint
        ERROR_VARIABLE _boot_key_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _boot_key_result EQUAL 0)
        message(FATAL_ERROR "${_boot_key_error}")
    endif()
    # Bind consumer compile identity to the validated PUBLIC bytes, not just
    # header mtimes: Make may see a rewritten header in the object's same second.
    target_compile_definitions(lm3s6965_boot_trust_root INTERFACE
        LM3S6965_BOOT_PHYSICAL_PRODUCTION=1
        LM3S6965_BOOT_PUBLIC_ROOT_SHA256="${_boot_fingerprint}")
    target_include_directories(lm3s6965_boot_trust_root INTERFACE "${_boot_root_dir}")
    message(STATUS "Production selected PUBLIC root SHA256: ${_boot_fingerprint} (authorization is external)")
else()
    message(STATUS "Boot trust: NONPRODUCTION repository demo PUBLIC root SHA256: f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d")
endif()
