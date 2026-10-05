#!/usr/bin/env bash
# Interactively test contrast through oled-brightness.sh; no MMIO here.
set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HELPER="${SCRIPT_DIRECTORY}/oled-brightness.sh"
BASELINE=0xB7

usage() {
    echo "Usage: scripts/oled-contrast-test.sh [--dry-run] START END STEP"
    echo "       scripts/oled-contrast-test.sh [--dry-run] --list VALUE [VALUE ...]"
    echo "       scripts/oled-contrast-test.sh --help"
    echo "Values: decimal or hex bytes (0-255). STEP is a positive byte magnitude."
    echo "Ranges ascend or descend; END is included only if reached exactly."
    echo "No default sweep. Real runs require a terminal and 'yes' before every write."
    echo "Requires an already initialized panel/SSI0 and OpenOCD; renders nothing."
    echo "Attempts vendor baseline 0xB7 restoration on exit after any write attempt."
    echo "Neither test values nor restoration guarantee panel safety."
}

fail() { echo "error: $*" >&2; exit 1; }

# Bound the digit count before arithmetic, including arbitrarily long input.
parse_byte() {
    local input="$1" digits base max_digits
    if [[ "$input" =~ ^0[xX][0-9a-fA-F]+$ ]]; then
        digits="${input:2}"; base=16; max_digits=2
    elif [[ "$input" =~ ^[0-9]+$ ]]; then
        digits="$input"; base=10; max_digits=3
    else
        fail "invalid byte '$input'; use decimal or 0x-prefixed hex."
    fi
    while [[ ${#digits} -gt 1 && "${digits:0:1}" == 0 ]]; do
        digits="${digits:1}"
    done
    [[ ${#digits} -le $max_digits ]] || fail "byte out of range: '$input'."
    BYTE=$((${base}#${digits}))
    (( BYTE <= 255 )) || fail "byte out of range: '$input'."
}

DRY_RUN=false
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
if [[ ${1:-} == --dry-run ]]; then
    DRY_RUN=true
    shift
fi
VALUES=()
if [[ ${1:-} == --list ]]; then
    shift
    (( $# > 0 )) || fail "--list needs at least one value."
    for input in "$@"; do
        parse_byte "$input"
        VALUES+=("$BYTE")
    done
else
    [[ $# -eq 3 ]] || { usage >&2; exit 1; }
    parse_byte "$1"; START=$BYTE
    parse_byte "$2"; END=$BYTE
    parse_byte "$3"; STEP=$BYTE
    (( STEP > 0 )) || fail "STEP must be greater than zero."
    if (( START <= END )); then
        for ((value=START; value<=END; value+=STEP)); do VALUES+=("$value"); done
    else
        for ((value=START; value>=END; value-=STEP)); do VALUES+=("$value"); done
    fi
fi

printf 'Requested contrast sequence:'
for value in "${VALUES[@]}"; do printf ' 0x%02X' "$value"; done
printf '\n'
echo "WARNING: 0x00-0xFF is the helper's byte test range, not a verified SSD1329 safe range." >&2
echo "0xB7 is the vendor initialization baseline, not a lifetime guarantee; 0xE0/0xFF are not safe ceilings." >&2
if $DRY_RUN; then
    echo "DRY RUN: NO hardware operations (including restoration)."
    echo "A real run asks before each step, then attempts restoration to $BASELINE."
    exit 0
fi

[[ -t 0 ]] || fail "real runs require terminal input; use --dry-run to preview."
[[ -x "$HELPER" ]] || fail "helper is not executable: $HELPER"
echo "Panel/SSI0 must already be initialized by the flashed image. Stop other debuggers."
echo "This script will not render content or diagnose a dark panel."
echo "Enter yes to apply each value; anything else (or Ctrl-C) stops the test."

RESTORE_NEEDED=false
cleanup() {
    local status=$?
    trap - EXIT
    trap '' INT TERM HUP
    if $RESTORE_NEEDED; then
        echo "Attempting baseline restoration to $BASELINE (no readback or safety guarantee)." >&2
        if ! "$HELPER" "$BASELINE"; then
            echo "WARNING: baseline restoration FAILED; panel state is unknown. Check the board before continuing." >&2
            (( status != 0 )) || status=1
        fi
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

for value in "${VALUES[@]}"; do
    printf -v hex_value '0x%02X' "$value"
    if (( value > 183 )); then
        echo "WARNING: $hex_value exceeds vendor baseline 0xB7; increased current may reduce panel life. Safety is unverified." >&2
    fi
    printf 'Apply %s? Type yes to proceed: ' "$hex_value"
    if ! IFS= read -r answer || [[ "$answer" != yes ]]; then
        echo "Stopped at user request."
        exit 0
    fi
    # Arm cleanup before calling the helper: failure may follow a partial write.
    RESTORE_NEEDED=true
    if "$HELPER" "$hex_value"; then
        echo "Inspect the display before approving the next step."
    else
        echo "error: contrast helper failed at $hex_value; stopping." >&2
        exit 1
    fi
done
