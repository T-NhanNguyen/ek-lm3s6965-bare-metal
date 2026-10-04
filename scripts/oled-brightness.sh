#!/usr/bin/env bash
#
# Set OLED contrast over SWD without a reset or a flash write.
# The firmware must have initialized the panel and SSI0.
#
# Usage:
#   ./scripts/oled-brightness.sh <value>
#   Use a hex byte such as 0xB7 or a decimal value from 0 to 255.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/openocd/board/ek-lm3s6965.cfg"

if [[ $# -ne 1 ]]; then
    echo "Usage: ./scripts/oled-brightness.sh <value>" >&2
    exit 1
fi

INPUT="$1"
if [[ "$INPUT" =~ ^0[xX][0-9a-fA-F]+$ ]]; then
    DIGITS="${INPUT:2}"
    BASE=16
    MAX_DIGITS=2
elif [[ "$INPUT" =~ ^[0-9]+$ ]]; then
    DIGITS="$INPUT"
    BASE=10
    MAX_DIGITS=3
else
    echo "error: use a hex byte or a decimal value from 0 to 255." >&2
    exit 1
fi

# Remove leading zeros before conversion to prevent integer overflow.
while [[ ${#DIGITS} -gt 1 && "${DIGITS:0:1}" == 0 ]]; do
    DIGITS="${DIGITS:1}"
done
if [[ ${#DIGITS} -gt $MAX_DIGITS ]]; then
    echo "error: value must be in the range 0x00-0xFF (0-255)." >&2
    exit 1
fi
VALUE=$((${BASE}#${DIGITS}))
if (( VALUE > 255 )); then
    echo "error: value must be in the range 0x00-0xFF (0-255)." >&2
    exit 1
fi
printf -v HEX_VALUE '0x%02X' "$VALUE"
echo "OLED contrast: $HEX_VALUE"
if (( VALUE > 224 )); then
    echo "CAUTION: Above the recommended ceiling of 0xE0. High current can reduce panel life." >&2
elif (( VALUE > 183 )); then
    echo "CAUTION: Above the vendor default of 0xB7. Higher contrast increases panel current." >&2
fi

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH." >&2
    exit 1
fi
if [[ ! -f "$BOARD_CONFIG" ]]; then
    echo "error: board config missing: $BOARD_CONFIG" >&2
    exit 1
fi

# Drain RX while polling BSY. A full RX FIFO can prevent SSI from going idle.
# The PC7 masked alias leaves the PC6 panel power signal unchanged.
exec openocd -f "$BOARD_CONFIG" \
    -c 'proc oled_wait_idle {} {
        for {set n 0} {$n < 100} {incr n} {
            for {set r 0} {$r < 8} {incr r} {
                set sr [lindex [read_memory 0x4000800C 32 1] 0]
                if {($sr & 4) == 0} {break}
                read_memory 0x40008008 32 1
            }
            set sr [lindex [read_memory 0x4000800C 32 1] 0]
            if {($sr & 0x10) == 0} {return}
            sleep 1
        }
        error {SSI0 did not return to idle.}
    }' \
    -c "init" \
    -c "halt" \
    -c "oled_wait_idle" \
    -c "mww 0x40006200 0x00000000" \
    -c "mww 0x40008008 0x00000081" \
    -c "mww 0x40008008 $HEX_VALUE" \
    -c "oled_wait_idle" \
    -c "echo {SSI0 is idle. The panel has no readback.}" \
    -c "resume" \
    -c "shutdown"
