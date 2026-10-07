#!/usr/bin/env bash
# OFFLINE only: native fixtures, Arm link probe, mocked Python, raw --self-test.
set -euo pipefail

usage() {
    printf '%s\n' \
        'Usage: scripts/ethernet/test.sh [all|ftp|SUITE ...]' \
        '       scripts/ethernet/test.sh --help | --list' \
        'Default: all offline suites. ftp = core tcp lwip consumer.' \
        'Suites: core tcp lwip file platform consumer driver raw link' \
        'No download, device access, live capture, flash, or host configuration.' \
        'consumer requires an installed Arm toolchain; raw requires macOS SDK.'
}
list_suites() {
    printf '%s\n' core tcp lwip file platform consumer driver raw link
}
# Validate the entire CLI before root discovery, temporary files or any build.
if (( $# == 1 )); then
    case "$1" in
        --help) usage; exit 0 ;;
        --list) list_suites; exit 0 ;;
    esac
fi
for arg in "$@"; do
    case "$arg" in
        all|ftp|core|tcp|lwip|file|platform|consumer|driver|raw|link) ;;
        *) printf 'Unknown suite or option: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done
SUITES=()
if (( $# == 0 )); then set -- all; fi
for arg in "$@"; do
    case "$arg" in
        all) SUITES+=(core tcp lwip file platform consumer driver raw link) ;;
        ftp) SUITES+=(core tcp lwip consumer) ;;
        *) SUITES+=("$arg") ;;
    esac
done

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/ethernet-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT
CC=${CC:-cc}
NATIVE=(-std=c11 -g -O1 -Wall -Wextra -Werror)
SANITIZERS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
STRICT=(-Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
INCLUDES=(-I"$ROOT/include")
LWIP_INCLUDES=(-I"$ROOT/scripts/ethernet/platform/shims"
               -I"$ROOT/examples/ethernet-ftp" -I"$ROOT/third_party/lwip/src/include")
LWIP_FLAGS=(-DLWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1)
# Preserve the original platform-only flag override, without eval.
PLATFORM_FLAGS=("${SANITIZERS[@]}")
if [[ -n ${FTP_PLATFORM_TEST_FLAGS:-} ]]; then
    read -r -a PLATFORM_FLAGS <<< "$FTP_PLATFORM_TEST_FLAGS"
fi
export UBSAN_OPTIONS=halt_on_error=1
# Apple's sanitizer runtime does not support LeakSanitizer.
if [[ $(uname -s) == Darwin ]]; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0"
fi
# Explicit pinned stack translation units previously selected by core/*.c and ipv4/*.c.
LWIP_SOURCES=(
    "$ROOT/third_party/lwip/src/core/altcp.c"
    "$ROOT/third_party/lwip/src/core/altcp_alloc.c"
    "$ROOT/third_party/lwip/src/core/altcp_tcp.c"
    "$ROOT/third_party/lwip/src/core/def.c"
    "$ROOT/third_party/lwip/src/core/dns.c"
    "$ROOT/third_party/lwip/src/core/inet_chksum.c"
    "$ROOT/third_party/lwip/src/core/init.c"
    "$ROOT/third_party/lwip/src/core/ip.c"
    "$ROOT/third_party/lwip/src/core/mem.c"
    "$ROOT/third_party/lwip/src/core/memp.c"
    "$ROOT/third_party/lwip/src/core/netif.c"
    "$ROOT/third_party/lwip/src/core/pbuf.c"
    "$ROOT/third_party/lwip/src/core/raw.c"
    "$ROOT/third_party/lwip/src/core/stats.c"
    "$ROOT/third_party/lwip/src/core/sys.c"
    "$ROOT/third_party/lwip/src/core/tcp.c"
    "$ROOT/third_party/lwip/src/core/tcp_in.c"
    "$ROOT/third_party/lwip/src/core/tcp_out.c"
    "$ROOT/third_party/lwip/src/core/timeouts.c"
    "$ROOT/third_party/lwip/src/core/udp.c"
    "$ROOT/third_party/lwip/src/core/ipv4/acd.c"
    "$ROOT/third_party/lwip/src/core/ipv4/autoip.c"
    "$ROOT/third_party/lwip/src/core/ipv4/dhcp.c"
    "$ROOT/third_party/lwip/src/core/ipv4/etharp.c"
    "$ROOT/third_party/lwip/src/core/ipv4/icmp.c"
    "$ROOT/third_party/lwip/src/core/ipv4/igmp.c"
    "$ROOT/third_party/lwip/src/core/ipv4/ip4.c"
    "$ROOT/third_party/lwip/src/core/ipv4/ip4_addr.c"
    "$ROOT/third_party/lwip/src/core/ipv4/ip4_frag.c"
    "$ROOT/third_party/lwip/src/netif/ethernet.c"
)
require_lwip() {
    if [[ ! -f "$ROOT/third_party/lwip/src/include/lwip/init.h" ]]; then
        printf '%s\n' 'lwIP submodule absent; no automatic download' >&2
        exit 1
    fi
}
core() {
    "$CC" "${NATIVE[@]}" "${SANITIZERS[@]}" "${STRICT[@]}" "${INCLUDES[@]}" \
        "$ROOT/scripts/ethernet/ftp/tests/ftp_core_test.c" \
        "$ROOT/src/ftp_core.c" "$ROOT/src/ram_file.c" -o "$BUILD/core"
    "$BUILD/core"
}
tcp() {
    require_lwip
    "$CC" "${NATIVE[@]}" "${SANITIZERS[@]}" "${LWIP_FLAGS[@]}" \
        "${LWIP_INCLUDES[@]}" "${INCLUDES[@]}" \
        "$ROOT/scripts/ethernet/ftp/tests/ftp_tcp_test.c" \
        "$ROOT/src/ftp_core.c" "$ROOT/src/ram_file.c" -o "$BUILD/tcp"
    "$BUILD/tcp"
}
lwip() {
    require_lwip
    "$CC" "${NATIVE[@]}" "${SANITIZERS[@]}" "${LWIP_FLAGS[@]}" \
        "${LWIP_INCLUDES[@]}" "${INCLUDES[@]}" \
        "$ROOT/scripts/ethernet/ftp/tests/ftp_lwip_test.c" \
        "$ROOT/src/ftp_core.c" "$ROOT/src/ftp_tcp.c" "$ROOT/src/ram_file.c" \
        "${LWIP_SOURCES[@]}" -o "$BUILD/lwip"
    ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0" "$BUILD/lwip"
}
file() {
    "$CC" "${NATIVE[@]}" "${SANITIZERS[@]}" "${STRICT[@]}" "${INCLUDES[@]}" \
        "$ROOT/scripts/ethernet/file/tests/ram_file_test.c" "$ROOT/src/ram_file.c" \
        -o "$BUILD/file"
    "$BUILD/file"
}
platform() {
    require_lwip
    "$CC" "${NATIVE[@]}" "${PLATFORM_FLAGS[@]}" "${LWIP_FLAGS[@]}" \
        "${LWIP_INCLUDES[@]}" -I"$ROOT/include/lm3s6965" \
        "$ROOT/scripts/ethernet/platform/tests/ftp_platform_test.c" \
        "${LWIP_SOURCES[@]}" -o "$BUILD/platform"
    "$BUILD/platform"
    "$CC" "${NATIVE[@]}" "${PLATFORM_FLAGS[@]}" \
        "$ROOT/scripts/ethernet/platform/tests/ftp_sbrk_test.c" \
        "$ROOT/examples/ethernet-ftp/ftp_sbrk.c" -o "$BUILD/heap"
    "$BUILD/heap"
    # CLI fixture uses only help/list/invalid arguments; never recurses into all.
    PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
        -s "$ROOT/scripts/ethernet/platform/tests" -p 'runner_cli_test.py' -v
}
consumer() {
    require_lwip
    mkdir -p "$BUILD/bsp"
    cp -R "$ROOT/src" "$ROOT/include" "$BUILD/bsp/"
    cp -R "$ROOT/scripts/ethernet/ftp/tests/ftp-library-consumer" "$BUILD/caller"
    cmake -S "$BUILD/caller" -B "$BUILD/consumer-build" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-lm3s6965.cmake" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBSP_ROOT="$BUILD/bsp" -DLWIP_ROOT="$ROOT/third_party/lwip"
    cmake --build "$BUILD/consumer-build" --target consumer --parallel 4
    if grep -q 'examples/' "$BUILD/consumer-build/compile_commands.json"; then
        printf '%s\n' 'ERROR: example include dependency' >&2; exit 1
    fi
    local size
    size=$(grep '^LM3S6965_SIZE:FILEPATH=' "$BUILD/consumer-build/CMakeCache.txt" | cut -d= -f2-)
    "$size" "$BUILD/consumer-build/consumer"
    printf '%s\n' 'External caller: strict Arm compile/link and transitive custom lwIP contract passed (src/include only).'
}
driver() {
    "$CC" "${NATIVE[@]}" "${SANITIZERS[@]}" -I"$ROOT/include/lm3s6965" \
        "$ROOT/scripts/ethernet/platform/tests/ethernet_test.c" -o "$BUILD/driver"
    "$BUILD/driver"
}
raw() {
    # Never call this live runner without its explicit offline mode.
    "$ROOT/scripts/ethernet/raw/ethernet-raw-test.sh" --self-test
}
link() {
    # Tests mock capture_runner, Popen and pyftdi; the shell fixture uses --capture-file.
    PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
        -s "$ROOT/scripts/ethernet/link/tests" -p '*_test.py' -v
}
COMPLETED=' '
for suite in "${SUITES[@]}"; do
    # Run overlapping aggregates only once, in first-requested order.
    if [[ $COMPLETED == *" $suite "* ]]; then continue; fi
    COMPLETED+="$suite "
    printf '\n=== Offline suite: %s ===\n' "$suite"
    case "$suite" in
        core) core ;; tcp) tcp ;; lwip) lwip ;; file) file ;;
        platform) platform ;; consumer) consumer ;; driver) driver ;;
        raw) raw ;; link) link ;;
    esac
done
