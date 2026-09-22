#!/usr/bin/env bash
#
# Capture the UART0 console from an EK-LM3S6965 through a USB-serial adapter.
#
# Usage:
#   scripts/console.sh [--device /dev/cu.usbserial-XXXX] [--baud 115200]
#                      [--seconds 5] [--no-reset]
#
# The firmware prints its banner exactly once, from main(), immediately after
# reset. A capture therefore only succeeds if the port is already being read
# BEFORE the target resets. This script starts the reader first and resets the
# target second; getting that order wrong is the usual cause of an empty
# capture.
#
# Wiring -- board UART0 header to adapter:
#   PA1 (UART0 TX)  ->  adapter RXD
#   PA0 (UART0 RX)  ->  adapter TXD
#   GND             ->  adapter GND
#   115200 baud, 8 data bits, no parity, 1 stop bit, no flow control.
#
# Why an external adapter: this board's on-board ICDI does not route its second
# FT2232 channel to MCU UART0, and macOS cannot bind its FTDI VCP dext to
# 0x0403:0xbcd9 because that product ID is absent from the driver's whitelist.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/openocd/board/ek-lm3s6965.cfg"

DEFAULT_BAUD_RATE=115200
DEFAULT_CAPTURE_SECONDS=5
READER_STARTUP_GRACE_SECONDS=0.3
ADAPTER_DEVICE_PATTERN='/dev/cu\.(usbserial|usbmodem|SLAB_USBtoUART|wchusbserial|USBtoUART)'
BANNER_MARKER="bring-up complete"

DEVICE_PATH=""
BAUD_RATE="$DEFAULT_BAUD_RATE"
CAPTURE_SECONDS="$DEFAULT_CAPTURE_SECONDS"
RESET_TARGET=true

usage() {
    cat <<'USAGE'
usage: scripts/console.sh [options]

  --device PATH    serial device to read (default: auto-detect)
  --baud RATE      baud rate (default: 115200)
  --seconds N      capture window in seconds (default: 5)
  --no-reset       do not reset the target (capture only)
  -h, --help       show this help

The banner is printed once at reset, so the reader is started first and the
target is reset second.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) DEVICE_PATH="${2:-}"; shift 2 ;;
        --baud) BAUD_RATE="${2:-}"; shift 2 ;;
        --seconds) CAPTURE_SECONDS="${2:-}"; shift 2 ;;
        --no-reset) RESET_TARGET=false; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$DEVICE_PATH" ]]; then
    detected_devices=$(ls /dev/cu.* 2>/dev/null | grep -E "$ADAPTER_DEVICE_PATTERN" || true)
    device_count=$(printf '%s' "$detected_devices" | grep -c . || true)

    if [[ "$device_count" -eq 0 ]]; then
        echo "error: no USB-serial adapter found." >&2
        echo >&2
        echo "Attach the adapter to the board's UART0 header:" >&2
        echo "  PA1 (UART0 TX) -> adapter RXD" >&2
        echo "  PA0 (UART0 RX) -> adapter TXD" >&2
        echo "  GND            -> adapter GND" >&2
        echo >&2
        echo "Devices currently present:" >&2
        ls /dev/cu.* 2>/dev/null | sed 's/^/  /' >&2
        exit 1
    fi

    if [[ "$device_count" -gt 1 ]]; then
        echo "error: more than one adapter found -- choose one with --device:" >&2
        printf '%s\n' "$detected_devices" | sed 's/^/  /' >&2
        exit 1
    fi

    DEVICE_PATH="$detected_devices"
fi

if [[ ! -e "$DEVICE_PATH" ]]; then
    echo "error: no such device: $DEVICE_PATH" >&2
    exit 1
fi

CAPTURE_FILE=$(mktemp)
trap 'rm -f "$CAPTURE_FILE"' EXIT

stty -f "$DEVICE_PATH" "$BAUD_RATE" cs8 -cstopb -parenb -crtscts -ixon -ixoff clocal raw -echo

echo "Listening on $DEVICE_PATH at ${BAUD_RATE} 8N1 ..."

cat "$DEVICE_PATH" > "$CAPTURE_FILE" &
CAPTURE_PID=$!

sleep "$READER_STARTUP_GRACE_SECONDS"

if [[ "$RESET_TARGET" == true ]]; then
    echo "Resetting the target -- the banner is emitted once, right now."
    openocd -f "$BOARD_CONFIG" -c "init" -c "reset run" -c "shutdown" >/dev/null 2>&1 || true
fi

sleep "$CAPTURE_SECONDS"

kill "$CAPTURE_PID" 2>/dev/null || true
wait "$CAPTURE_PID" 2>/dev/null || true

captured_bytes=$(wc -c < "$CAPTURE_FILE" | tr -d ' ')

echo
echo "Captured $captured_bytes bytes from $DEVICE_PATH"
echo "----------------------------------------------"

if [[ "$captured_bytes" -eq 0 ]]; then
    echo "(nothing received)"
    echo "----------------------------------------------"
    echo "Checks:"
    echo "  - Are TX and RX the right way round? PA1 must reach the adapter's RXD."
    echo "  - Is GND shared between the board and the adapter?"
    echo "  - Is --device pointing at the right port?"
    exit 1
fi

cat "$CAPTURE_FILE"
echo
echo "----------------------------------------------"

if grep -q "$BANNER_MARKER" "$CAPTURE_FILE"; then
    echo "PASS: UART0 console received over $DEVICE_PATH"
    exit 0
fi

echo "WARNING: bytes arrived, but the marker '$BANNER_MARKER' was not found."
echo "         Garbage above usually means the baud rate is wrong."
exit 1
