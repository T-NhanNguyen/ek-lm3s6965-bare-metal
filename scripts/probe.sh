#!/usr/bin/env bash
#
# Read-only connectivity check for the EK-LM3S6965.
# Resets and halts the core, reports identity and flash geometry, then resumes.
# Writes nothing to flash — use this before trusting flash.sh.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/openocd/board/ek-lm3s6965.cfg"

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH — install with 'brew install openocd'" >&2
    exit 1
fi

if [[ ! -f "$BOARD_CONFIG" ]]; then
    echo "error: board config missing: $BOARD_CONFIG" >&2
    exit 1
fi

echo "Probing EK-LM3S6965 over ICDI (read-only, no flash writes)..."
echo

if ! openocd -f "$BOARD_CONFIG" \
        -c "init" \
        -c "reset halt" \
        -c "echo {--- device identity: DID0 then DID1 ---}" \
        -c "mdw 0x400FE000 1" \
        -c "mdw 0x400FE004 1" \
        -c "echo {--- vector table: MSP then Reset_Handler ---}" \
        -c "mdw 0x00000000 2" \
        -c "echo {--- flash banks ---}" \
        -c "flash banks" \
        -c "resume" \
        -c "shutdown"
then
    echo >&2
    echo "Connection failed. Diagnose by layer:" >&2
    echo "  'no device found' / 'unable to open ftdi device' -> cable or probe not present." >&2
    echo "      Run scripts/check-connection.sh to see what the host detects." >&2
    echo "  'JTAG scan chain interrogation failed'         -> probe seen, target not responding." >&2
    echo "      Check board power and the JTAG ribbon." >&2
    exit 1
fi
