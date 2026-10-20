cmake_minimum_required(VERSION 3.20)
project(ftp_fixture_pattern LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
enable_testing()
foreach(pattern IN ITEMS 1 2)
    add_executable(fixture_pattern_${pattern} pattern_test.c
        "${ROOT}/examples/ethernet-ftp/fixture_pattern.c"
        "${ROOT}/examples/ethernet-ftp/fixture_led.c")
    target_include_directories(fixture_pattern_${pattern} PRIVATE
        "${ROOT}/examples/ethernet-ftp" "${ROOT}/include/lm3s6965")
    target_compile_definitions(fixture_pattern_${pattern} PRIVATE
        LM3S6965_FTP_FOREGROUND_OWNER=1 LM3S6965_FTP_FIXTURE_PATTERN=${pattern})
    target_compile_options(fixture_pattern_${pattern} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME fixture_pattern_${pattern} COMMAND fixture_pattern_${pattern})
endforeach()
