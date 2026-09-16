#!/usr/bin/env bash
#
# Flash the built firmware to an attached EK-LM3S6965 over the on-board ICDI probe.
#
# Usage:
#   scripts/flash.sh [path/to/firmware.elf]
#
# Defaults to build/lm3s6965_firmware. Verifies after programming and resets.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/openocd/board/ek-lm3s6965.cfg"
FIRMWARE_ELF="${1:-${REPOSITORY_ROOT}/build/lm3s6965_firmware}"

if [[ ! -f "$FIRMWARE_ELF" ]]; then
    echo "error: firmware not found: $FIRMWARE_ELF" >&2
    echo "build it first:" >&2
    echo "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake" >&2
    echo "  cmake --build build" >&2
    exit 1
fi

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH — install with 'brew install openocd'" >&2
    exit 1
fi

echo "Flashing $(basename "$FIRMWARE_ELF") to EK-LM3S6965..."
echo

exec openocd -f "$BOARD_CONFIG" \
    -c "program $(realpath "$FIRMWARE_ELF") verify reset exit"
