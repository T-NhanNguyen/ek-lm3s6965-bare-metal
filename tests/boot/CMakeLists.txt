cmake_minimum_required(VERSION 3.20)
project(boot_format_host LANGUAGES C)

# Standalone host project: deliberately does not include the BSP/ARM build.
option(BOOT_SANITIZERS "Enable host AddressSanitizer and UndefinedBehaviorSanitizer" ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(../../cmake/boot_crypto.cmake)
add_library(boot_verify STATIC ../../src/boot_verify.c ../../src/boot_format.c)
target_include_directories(boot_verify PUBLIC ../../include)
target_link_libraries(boot_verify PUBLIC boot_crypto)
target_compile_options(boot_verify PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_verify PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_store STATIC ../../src/boot_store.c ../../src/boot_format.c)
target_include_directories(boot_store PUBLIC ../../include)
target_compile_options(boot_store PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_store PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_lifecycle STATIC ../../src/boot_lifecycle.c ../../src/boot_format.c)
target_include_directories(boot_lifecycle PUBLIC ../../include)
target_compile_options(boot_lifecycle PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_lifecycle PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_reset STATIC ../../src/boot_reset.c)
target_link_libraries(boot_reset PUBLIC boot_lifecycle)
target_compile_options(boot_reset PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_reset PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_reset_handler STATIC ../../src/boot_reset_handler.c)
target_link_libraries(boot_reset_handler PUBLIC boot_reset boot_store)
target_compile_options(boot_reset_handler PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_reset_handler PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_diag STATIC ../../src/boot_diag.c)
target_include_directories(boot_diag PUBLIC ../../include)
target_compile_options(boot_diag PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_diag PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_diag_publication STATIC ../../src/boot_diag_publication.c)
target_link_libraries(boot_diag_publication PUBLIC boot_diag)
target_compile_options(boot_diag_publication PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_diag_publication PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_snapshot STATIC ../../src/boot_snapshot.c)
target_link_libraries(boot_snapshot PUBLIC boot_diag_publication boot_store)
target_compile_options(boot_snapshot PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_snapshot PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_flash STATIC ../../src/boot_flash.c)
target_link_libraries(boot_flash PUBLIC boot_store)
target_compile_options(boot_flash PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_flash PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_watchdog STATIC ../../src/boot_watchdog.c)
target_include_directories(boot_watchdog PUBLIC ../../include)
target_compile_options(boot_watchdog PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_watchdog PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_handoff STATIC ../../src/boot_handoff.c)
target_link_libraries(boot_handoff PUBLIC boot_watchdog)
target_compile_options(boot_handoff PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_handoff PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_launch STATIC ../../src/boot_launch.c ../../src/boot_select.c)
target_link_libraries(boot_launch PUBLIC boot_verify boot_store boot_lifecycle)
target_compile_options(boot_launch PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_launch PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_runtime STATIC ../../src/boot_runtime.c)
target_link_libraries(boot_runtime PUBLIC boot_reset_handler boot_launch)
target_compile_options(boot_runtime PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_runtime PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
add_library(boot_runtime_watchdog STATIC ../../src/boot_runtime_watchdog.c)
target_link_libraries(boot_runtime_watchdog PUBLIC boot_runtime boot_watchdog)
target_compile_options(boot_runtime_watchdog PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_compile_options(boot_runtime_watchdog PRIVATE -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -fno-common
        $<$<CONFIG:Release>:-Os>)
endif()
# The same standalone project is the cross-build entry point for the archive.
# Native test executables are deliberately not built for bare-metal ARM.
if(CMAKE_CROSSCOMPILING)
    return()
endif()
enable_testing()
enable_language(ASM)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME boot_dependencies COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dependencies.py -v)
set_tests_properties(boot_dependencies PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
# Fixed synthetic linker symbols exercise both complete production units.
# The 2048-byte full/ftp/bad_* cases are nonproduction capacity fixtures;
# real linked FTP replays below enforce the approved 8192-byte total reserve.
set(SBRK_FIXTURES
    "zero,0,0,0"
    "zero_heap,0,0,16"
    "empty,0x20000030,0x20000030,0x2000dfc0"
    "static,0x20000030,0x20000050,0x2000dfc0"
    "full,0x20000030,0x20000830,0x20000830"
    "ftp,0x20000030,0x20000130,0x20000830"
    "bad_start,0x20000051,0x20000050,0x20000830"
    "bad_end,0x20000030,0x20000831,0x20000830")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND SBRK_FIXTURES "wrap,0xfffffffffffff000,0xfffffffffffff010,0xfffffffffffffff0")
else()
    list(APPEND SBRK_FIXTURES "wrap,0xfffff000,0xfffff010,0xfffffff0")
endif()
set(SBRK_PROGRAMS "")
foreach(kind IN ITEMS generic ftp)
    foreach(fixture IN LISTS SBRK_FIXTURES)
        string(REPLACE "," ";" fields "${fixture}")
        list(GET fields 0 name)
        list(GET fields 1 start)
        list(GET fields 2 base)
        list(GET fields 3 end)
        set(target "test_sbrk_${kind}_${name}")
        add_executable(${target} test_sbrk.c sbrk_fixture.c sbrk_symbols.S)
        target_include_directories(${target} PRIVATE ../../include/lm3s6965)
        target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE
            TEST_START=${start} TEST_BASE=${base} TEST_END=${end})
        if(kind STREQUAL "generic")
            target_compile_definitions(${target} PRIVATE TEST_GENERIC)
        endif()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-Wpedantic;-Wconversion;-Wsign-conversion;-Wshadow;-Wstrict-prototypes;-Wmissing-prototypes>)
        if(BOOT_SANITIZERS)
            target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:C>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        list(APPEND SBRK_PROGRAMS "$<TARGET_FILE:${target}>")
    endforeach()
endforeach()
add_test(NAME boot_allocator COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sbrk_linked.py --cc ${CMAKE_C_COMPILER}
    --sanitizers ${BOOT_SANITIZERS} ${SBRK_PROGRAMS})
add_executable(test_handoff test_handoff.c)
target_link_libraries(test_handoff PRIVATE boot_handoff)
target_compile_options(test_handoff PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_handoff COMMAND test_handoff)
add_test(NAME boot_handoff_target COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_handoff_target.py)
set_tests_properties(boot_handoff_target PROPERTIES TIMEOUT 120
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_executable(test_watchdog test_watchdog.c)
target_link_libraries(test_watchdog PRIVATE boot_watchdog)
target_compile_options(test_watchdog PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_watchdog COMMAND test_watchdog)
add_executable(test_flash test_flash.c)
target_link_libraries(test_flash PRIVATE boot_flash)
target_compile_options(test_flash PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_flash COMMAND test_flash)
add_executable(test_snapshot test_snapshot.c)
target_link_libraries(test_snapshot PRIVATE boot_snapshot)
target_compile_options(test_snapshot PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wpedantic)
add_executable(test_diag_publication test_diag_publication.c)
target_link_libraries(test_diag_publication PRIVATE boot_diag_publication)
# Assertions are the test oracle even in Release.
target_compile_options(test_diag_publication PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wpedantic)
add_test(NAME boot_diag_publication COMMAND test_diag_publication)
add_test(NAME boot_diag_publication_target COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diag_publication_target.py)
set_tests_properties(boot_diag_publication_target PROPERTIES TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_executable(test_diag test_diag.c)
target_link_libraries(test_diag PRIVATE boot_diag)
target_compile_options(test_diag PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_diag COMMAND test_diag)
add_executable(test_reset_handler test_reset_handler.c flash_model.c)
target_link_libraries(test_reset_handler PRIVATE boot_reset_handler)
target_compile_options(test_reset_handler PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_reset_handler COMMAND test_reset_handler)
add_executable(test_reset test_reset.c flash_model.c)
target_link_libraries(test_reset PRIVATE boot_reset boot_store)
target_compile_options(test_reset PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_reset COMMAND test_reset)
add_executable(test_lifecycle test_lifecycle.c flash_model.c)
target_link_libraries(test_lifecycle PRIVATE boot_lifecycle boot_store)
target_compile_options(test_lifecycle PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_lifecycle COMMAND test_lifecycle)
add_executable(test_store test_store.c flash_model.c)
target_link_libraries(test_store PRIVATE boot_store)
target_compile_options(test_store PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_store COMMAND test_store)
add_executable(test_format test_format.c ../../src/boot_format.c)
target_include_directories(test_format PRIVATE ../../include)
target_compile_options(test_format PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE boot_crypto)
target_compile_options(test_crypto PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_executable(test_verify test_verify.c)
target_link_libraries(test_verify PRIVATE boot_verify)
target_compile_options(test_verify PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_executable(package_verify package_verify.c)
target_link_libraries(package_verify PRIVATE boot_verify)
target_compile_options(package_verify PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
add_test(NAME boot_package COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_package.py --verifier $<TARGET_FILE:package_verify> -v)
# Real GNU Arm fixture producers use the three new partition scripts and both
# established standalone scripts. Missing Arm tools fail rather than skip.
add_test(NAME boot_layout COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_layout.py)
set_tests_properties(boot_layout PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(NAME boot_resources COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_resource.py)
set_tests_properties(boot_resources PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(NAME boot_resource_execution COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_resource_execution.py)
# An optional unavailable emulator is explicit UNSUPPORTED, never a pass.
set_tests_properties(boot_resource_execution PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
# Test-only allocator interception: compile an isolated copy, never alter the
# reviewed crypto target/config or expose fault controls in the production API.
get_target_property(BOOT_FAULT_SOURCES boot_crypto SOURCES)
add_library(boot_crypto_fault STATIC ${BOOT_FAULT_SOURCES})
target_link_libraries(boot_crypto_fault PUBLIC boot_crypto)
target_compile_definitions(boot_crypto_fault PRIVATE
    calloc=boot_verify_test_calloc free=boot_verify_test_free)
add_library(boot_verify_fault STATIC ../../src/boot_verify.c ../../src/boot_format.c)
target_include_directories(boot_verify_fault PUBLIC ../../include)
target_link_libraries(boot_verify_fault PUBLIC boot_crypto_fault)
add_executable(test_verify_fault test_verify.c)
target_compile_definitions(test_verify_fault PRIVATE BOOT_VERIFY_FAULT_INJECTION)
target_link_libraries(test_verify_fault PRIVATE boot_verify_fault)
foreach(target IN ITEMS boot_verify_fault test_verify_fault)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
endforeach()
if(BOOT_SANITIZERS)
    foreach(target IN ITEMS boot_runtime_watchdog boot_runtime test_handoff boot_handoff test_watchdog boot_watchdog test_flash boot_flash test_snapshot boot_snapshot test_diag_publication boot_diag_publication test_diag boot_diag test_reset_handler boot_reset_handler test_reset boot_reset test_lifecycle boot_lifecycle test_store boot_store test_format test_crypto test_verify boot_verify boot_crypto
                            boot_crypto_fault boot_verify_fault test_verify_fault package_verify)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    foreach(target IN ITEMS test_handoff test_watchdog test_flash test_snapshot test_diag_publication test_diag test_reset_handler test_reset test_lifecycle test_store test_format test_crypto test_verify test_verify_fault package_verify)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
add_test(NAME boot_format COMMAND test_format)
add_test(NAME boot_crypto COMMAND test_crypto)
add_test(NAME boot_verify COMMAND test_verify)
add_test(NAME boot_verify_fault COMMAND test_verify_fault)
add_test(NAME boot_snapshot COMMAND test_snapshot)
# Launch host tests reuse the established normal and fault-injected targets.
add_subdirectory(../boot_launch boot_launch)
# Apple ASan has no LeakSanitizer; crypto tests do allocate/free MPI storage.
set(BOOT_ASAN_OPTIONS "halt_on_error=1")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(APPEND BOOT_ASAN_OPTIONS ":detect_leaks=1")
endif()
set_tests_properties(boot_handoff boot_watchdog boot_flash boot_snapshot boot_diag_publication boot_allocator boot_diag boot_reset_handler boot_reset boot_lifecycle boot_store boot_format boot_crypto boot_verify boot_verify_fault boot_package PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=${BOOT_ASAN_OPTIONS};UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;PYTHONDONTWRITEBYTECODE=1")
