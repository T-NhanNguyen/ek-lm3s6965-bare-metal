# Supported host-test subdirectory of tests/boot; not a standalone wrapper.
# Reuse its real verifier/crypto/store/lifecycle targets and sanitizer policy.
add_executable(test_launch test_launch.c ../boot/flash_model.c)
target_include_directories(test_launch PRIVATE ../boot)
target_link_libraries(test_launch PRIVATE boot_launch)
target_compile_options(test_launch PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
set_target_properties(test_launch PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME boot_launch COMMAND test_launch)
# Existing allocator-intercepted real crypto copy; no production fault API.
add_library(boot_launch_fault STATIC ../../src/boot_launch.c ../../src/boot_select.c)
target_link_libraries(boot_launch_fault PUBLIC boot_verify_fault boot_store boot_lifecycle)
add_executable(test_launch_fault test_launch.c ../boot/flash_model.c)
target_include_directories(test_launch_fault PRIVATE ../boot)
target_compile_definitions(test_launch_fault PRIVATE BOOT_VERIFY_FAULT_INJECTION)
target_link_libraries(test_launch_fault PRIVATE boot_launch_fault)
foreach(target IN ITEMS boot_launch_fault test_launch_fault)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()
add_test(NAME boot_launch_fault COMMAND test_launch_fault)
foreach(variant IN ITEMS "" "_fault")
    add_executable(test_select${variant} test_select.c ../boot/flash_model.c)
    target_include_directories(test_select${variant} PRIVATE ../boot)
    target_link_libraries(test_select${variant} PRIVATE boot_launch${variant})
    target_compile_options(test_select${variant} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    set_target_properties(test_select${variant} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME boot_select${variant} COMMAND test_select${variant})
endforeach()
target_compile_definitions(test_select_fault PRIVATE BOOT_VERIFY_FAULT_INJECTION)
foreach(variant IN ITEMS "" "_fault")
    if(variant STREQUAL "_fault")
        add_library(boot_runtime_fault STATIC ../../src/boot_runtime.c)
        target_link_libraries(boot_runtime_fault PUBLIC boot_reset_handler boot_launch_fault)
        target_compile_options(boot_runtime_fault PRIVATE
            -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    endif()
    add_executable(test_runtime${variant} test_runtime.c ../boot/flash_model.c)
    target_include_directories(test_runtime${variant} PRIVATE ../boot)
    target_link_libraries(test_runtime${variant} PRIVATE boot_runtime${variant})
    target_compile_options(test_runtime${variant} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    add_test(NAME boot_runtime${variant} COMMAND test_runtime${variant})
endforeach()
target_compile_definitions(test_runtime_fault PRIVATE BOOT_VERIFY_FAULT_INJECTION)
foreach(variant IN ITEMS "" "_fault")
    if(variant STREQUAL "_fault")
        add_library(boot_runtime_watchdog_fault STATIC ../../src/boot_runtime_watchdog.c)
        target_link_libraries(boot_runtime_watchdog_fault PUBLIC boot_runtime_fault boot_watchdog)
        target_compile_options(boot_runtime_watchdog_fault PRIVATE
            -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    endif()
    add_executable(test_runtime_watchdog${variant} test_runtime_watchdog.c ../boot/flash_model.c)
    target_include_directories(test_runtime_watchdog${variant} PRIVATE ../boot)
    target_link_libraries(test_runtime_watchdog${variant} PRIVATE boot_runtime_watchdog${variant})
    target_compile_options(test_runtime_watchdog${variant} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    add_test(NAME boot_runtime_watchdog${variant} COMMAND test_runtime_watchdog${variant})
endforeach()
target_compile_definitions(test_runtime_watchdog_fault PRIVATE BOOT_VERIFY_FAULT_INJECTION)
set(BOOT_LAUNCH_ASAN_OPTIONS "halt_on_error=1")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(APPEND BOOT_LAUNCH_ASAN_OPTIONS ":detect_leaks=1")
endif()
set_tests_properties(boot_runtime_watchdog boot_runtime_watchdog_fault boot_launch boot_launch_fault boot_select boot_select_fault boot_runtime boot_runtime_fault PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=${BOOT_LAUNCH_ASAN_OPTIONS};UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
if(BOOT_SANITIZERS)
    foreach(target IN ITEMS test_runtime_watchdog test_runtime_watchdog_fault boot_runtime_watchdog_fault test_launch boot_launch_fault test_launch_fault test_select test_select_fault test_runtime test_runtime_fault boot_runtime_fault)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(boot_runtime_watchdog INTERFACE -fsanitize=address,undefined)
    target_link_options(boot_runtime_watchdog_fault INTERFACE -fsanitize=address,undefined)
    target_link_options(boot_runtime INTERFACE -fsanitize=address,undefined)
    target_link_options(boot_runtime_fault INTERFACE -fsanitize=address,undefined)
    target_link_options(boot_launch_fault INTERFACE -fsanitize=address,undefined)
    target_compile_options(boot_launch PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(boot_launch INTERFACE -fsanitize=address,undefined)
endif()
