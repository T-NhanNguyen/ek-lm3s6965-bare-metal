#!/usr/bin/env bash
# Build native macOS host tools unprivileged; elevation is always explicit.
set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "${SCRIPT_DIRECTORY}/.." && pwd)
BUILD="${ROOT}/build/host"

usage() {
    printf '%s\n' \
        'Usage: scripts/ethernet-raw-test.sh [interface] (default en7)' \
        '       scripts/ethernet-raw-test.sh --build-only' \
        '       scripts/ethernet-raw-test.sh --self-test' \
        '       scripts/ethernet-raw-test.sh --help' \
        '' \
        'Default: builds then sends 3 exact 60-byte requests, 3s each.' \
        '--self-test: offline C tests with address/undefined sanitizers only.' \
        '--build-only: builds the helper without opening BPF or sending.' \
        'No automatic sudo, permissions installer or interface changes.' \
        '' \
        'If approved elevation is needed, build as your regular user first:' \
        '  scripts/ethernet-raw-test.sh --build-only' \
        '  sudo ./build/host/ethernet_raw_test en7' \
        'The helper drops to SUDO_UID/GID before the bounded packet loop.' \
        'The open BPF descriptor still permits raw frames after dropping.' \
        'PASS covers bidirectional 60-byte exchange, not IP/FTP/full MTU.'
}

if [[ "${1:-}" == --help ]]; then
    usage
    exit 0
fi
if (( $# > 1 )); then
    usage >&2
    exit 2
fi
MODE=${1:-en7}
case "${MODE}" in
    --build-only|--self-test) ;;
    -*) usage >&2; exit 2 ;;
esac
if (( EUID == 0 )); then
    printf '%s\n' 'Do not compile as root. Build unprivileged, then run the' \
        'already-built helper explicitly if elevation is approved.' >&2
    exit 1
fi
if [[ $(uname -s) != Darwin ]]; then
    printf '%s\n' 'This helper requires the actual macOS SDK BPF ABI.' >&2
    exit 1
fi
mkdir -p "${BUILD}"
SDK=$(xcrun --show-sdk-path)
CC=$(xcrun --find clang)
FLAGS=(-isysroot "${SDK}" -std=c11 -D_DARWIN_C_SOURCE
       -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow
       -Wstrict-prototypes -Wmissing-prototypes)
SHARED=("${ROOT}/tools/ethernet_raw_capture.c"
        "${ROOT}/examples/ethernet-raw/raw_protocol.c")
if [[ ${MODE} == --self-test ]]; then
    FLAGS+=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined)
    "${CC}" "${FLAGS[@]}" "${ROOT}/tests/ethernet_raw_capture_test.c" \
        "${SHARED[@]}" -o "${BUILD}/ethernet_raw_capture_test"
    "${CC}" "${FLAGS[@]}" "${ROOT}/tests/raw_protocol_test.c" \
        "${ROOT}/examples/ethernet-raw/raw_protocol.c" \
        -o "${BUILD}/raw_protocol_test"
    "${CC}" "${FLAGS[@]}" "${ROOT}/tests/ethernet_raw_privilege_test.c" \
        -o "${BUILD}/ethernet_raw_privilege_test"
    "${BUILD}/ethernet_raw_privilege_test"
    "${BUILD}/ethernet_raw_capture_test"
    exec "${BUILD}/raw_protocol_test"
fi
"${CC}" "${FLAGS[@]}" -O2 "${ROOT}/tools/ethernet_raw_test.c" \
    "${SHARED[@]}" "${ROOT}/tools/ethernet_raw_privilege.c" \
    -o "${BUILD}/ethernet_raw_test"
if [[ ${MODE} == --build-only ]]; then
    printf 'Built without BPF access: %s\n' "${BUILD}/ethernet_raw_test"
    printf '%s\n' 'Run as your user, or explicitly if approved:' \
        "  sudo \"${BUILD}/ethernet_raw_test\" en7"
    exit 0
fi
exec "${BUILD}/ethernet_raw_test" "${MODE}"
