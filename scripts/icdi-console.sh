#!/usr/bin/env bash
#
# Read the LM3S6965 console through the on-board ICDI, over SWO.
#
# Usage:
#   scripts/icdi-console.sh [--seconds N] [--baud RATE] [--raw]
#
# The firmware writes every printf byte to ITM stimulus port 0 as well as to
# UART0. The trace unit serialises those bytes out of the Cortex-M3 SWO pin, the
# board's CPLD taps that pin, and the ICDI's second FT2232 channel carries them.
# This script reads that channel directly over libusb -- no kernel driver.
#
# Two conditions hold, and this script handles both:
#   1. The probe must use SWD, because only then does the shared TDO pin carry
#      SWO. The board config defaults to SWD.
#   2. The reader must be listening BEFORE the target resets, because the
#      firmware prints its banner once, directly after reset.
#
# Needs pyftdi. Install it into a virtual environment:
#   python3 -m venv .venv && .venv/bin/pip install pyftdi
# The script looks in .venv, then at python3. Set PYFTDI_PYTHON to override.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/openocd/board/ek-lm3s6965.cfg"

DEFAULT_SWO_BAUD_RATE=1000000
DEFAULT_CAPTURE_SECONDS=4
BANNER_MARKER="bring-up complete"

SWO_BAUD_RATE="$DEFAULT_SWO_BAUD_RATE"
CAPTURE_SECONDS="$DEFAULT_CAPTURE_SECONDS"
SHOW_RAW=false

usage() {
    cat <<'USAGE'
usage: scripts/icdi-console.sh [options]

  --seconds N   capture window in seconds (default: 4)
  --baud RATE   SWO rate, must match the firmware (default: 1000000)
  --raw         print the raw captured bytes as hex as well
  -h, --help    show this help

Reads the console through the on-board ICDI over SWO. No serial adapter needed.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) CAPTURE_SECONDS="${2:-}"; shift 2 ;;
        --baud) SWO_BAUD_RATE="${2:-}"; shift 2 ;;
        --raw) SHOW_RAW=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

find_python_with_pyftdi() {
    local candidate
    for candidate in "${PYFTDI_PYTHON:-}" \
                     "${REPOSITORY_ROOT}/.venv/bin/python" \
                     "python3"; do
        if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1 &&
           "$candidate" -c "import pyftdi" >/dev/null 2>&1; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH -- install with 'brew install openocd'" >&2
    exit 1
fi

if ! PYTHON_INTERPRETER=$(find_python_with_pyftdi); then
    echo "error: no Python interpreter with pyftdi was found." >&2
    echo >&2
    echo "Install it into a virtual environment:" >&2
    echo "  python3 -m venv .venv" >&2
    echo "  .venv/bin/pip install pyftdi" >&2
    echo >&2
    echo "Or point PYFTDI_PYTHON at an interpreter that has it:" >&2
    echo "  PYFTDI_PYTHON=/path/to/python scripts/icdi-console.sh" >&2
    exit 1
fi

exec "$PYTHON_INTERPRETER" - "$BOARD_CONFIG" "$SWO_BAUD_RATE" "$CAPTURE_SECONDS" \
    "$SHOW_RAW" "$BANNER_MARKER" <<'PYTHON'
import collections
import subprocess
import sys
import threading
import time

from pyftdi.ftdi import Ftdi

BOARD_CONFIG, SWO_BAUD_RATE, CAPTURE_SECONDS, SHOW_RAW, BANNER_MARKER = (
    sys.argv[1], int(sys.argv[2]), float(sys.argv[3]),
    sys.argv[4] == "true", sys.argv[5])

ICDI_VENDOR_ID, ICDI_PRODUCT_ID = 0x0403, 0xbcd9
ICDI_SECOND_CHANNEL = 2
READER_STARTUP_GRACE_SECONDS = 0.4
STIMULUS_PORT_ZERO = 0
SOURCE_PACKET_BIT = 0x01
SIZE_CODE_SHIFT = 1
PORT_SHIFT = 3
USB_RETRY_ATTEMPTS = 5
USB_RETRY_DELAY_SECONDS = 0.6


class IcdiError(RuntimeError):
    pass


def retry_usb(description, operation, attempts=USB_RETRY_ATTEMPTS):
    """Run a USB control transfer, retrying on the transient FT2232D timeouts."""
    last_error = None
    for _ in range(attempts):
        try:
            return operation()
        except Exception as error:
            last_error = error
            time.sleep(USB_RETRY_DELAY_SECONDS)
    raise IcdiError("the ICDI failed to %s: %s" % (description, last_error))


def read_itm_stimulus_port_zero(data):
    """Walk the ITM packet stream and return the port 0 payload bytes.

    Each packet starts with a header byte. Bit 0 set means a source packet, so
    bits 7:3 hold the stimulus port and bits 2:1 hold a size code. Bit 0 clear
    means a protocol packet -- sync, overflow, or timestamp -- which carries no
    console text and is skipped.
    """
    text = bytearray()
    index = 0
    while index < len(data):
        header = data[index]
        index += 1
        if not (header & SOURCE_PACKET_BIT):
            continue
        size_code = (header >> SIZE_CODE_SHIFT) & 0x03
        if size_code == 0:
            continue
        payload_size = 1 << (size_code - 1)
        port = header >> PORT_SHIFT
        payload = data[index:index + payload_size]
        index += payload_size
        if port == STIMULUS_PORT_ZERO and payload:
            text.append(payload[0])
    return bytes(text)


captured = bytearray()
read_errors = []
stop_reading = threading.Event()


def reader(device):
    while not stop_reading.is_set():
        try:
            chunk = device.read_data(64)
        except Exception as error:
            read_errors.append(str(error))
            time.sleep(0.05)
            continue
        if chunk:
            captured.extend(chunk)


device = Ftdi()
try:
    retry_usb("open the second channel",
              lambda: device.open(ICDI_VENDOR_ID, ICDI_PRODUCT_ID,
                                  interface=ICDI_SECOND_CHANNEL))
except IcdiError as error:
    raise SystemExit(str(error))

time.sleep(0.3)

# Flush before touching the pin mode or the baud rate. Purging straight after
# set_baudrate times out on this FT2232D, and a flush is a nicety anyway: the
# decoder skips protocol packets and the text is matched against the marker.
try:
    retry_usb("flush its buffers", device.purge_buffers, attempts=2)
except IcdiError as error:
    print("warning: %s" % error)
    print("warning: continuing without a flush; leading bytes may be stale.")

try:
    retry_usb("reset the pin mode",
              lambda: device.set_bitmode(0x00, Ftdi.BitMode.RESET))
    device.timeouts = (1, 1)
    retry_usb("accept %d baud" % SWO_BAUD_RATE,
              lambda: device.set_baudrate(SWO_BAUD_RATE))
except IcdiError as error:
    raise SystemExit(str(error))

print("Listening on the ICDI at %d 8N1 ..." % SWO_BAUD_RATE)

reading_thread = threading.Thread(target=reader, args=(device,), daemon=True)
reading_thread.start()
time.sleep(READER_STARTUP_GRACE_SECONDS)

print("Resetting the target -- the banner is printed once, right now.")
print("The probe stays attached for the whole capture window, because the board's")
print("CPLD routes SWO only while the probe asserts its SWD_EN line.")

probe = subprocess.Popen(
    ["openocd", "-f", BOARD_CONFIG, "-c", "init", "-c", "reset"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(CAPTURE_SECONDS)

stop_reading.set()
reading_thread.join(timeout=2)

probe.terminate()
try:
    probe.wait(timeout=5)
except subprocess.TimeoutExpired:
    probe.kill()

try:
    device.close()
except Exception:
    pass

text = read_itm_stimulus_port_zero(captured)

print()
print("Captured %d bytes, decoded %d characters from ITM stimulus port 0."
      % (len(captured), len(text)))
if read_errors:
    print("warning: %d read errors; first was: %s"
          % (len(read_errors), read_errors[0]))
if SHOW_RAW:
    print("raw:", bytes(captured[:64]).hex(" "))
print("-" * 62)

if not text:
    print("(no console text)")
    print("-" * 62)
    print("Checks:")
    print("  - Did the probe stay attached for the whole capture? The board's CPLD")
    print("    routes SWO only while the probe asserts SWD_EN.")
    print("  - Is the probe in SWD mode? The board config defaults to swd.")
    print("  - Does --baud match SWO_BAUD_RATE in src/main.c?")
    print("  - Did the firmware call trace_initialize()?")
    raise SystemExit(1)

sys.stdout.write(text.decode("utf-8", errors="replace"))
if not text.endswith(b"\n"):
    print()
print("-" * 62)

if BANNER_MARKER in text.decode("utf-8", errors="replace"):
    print("PASS: console received over SWO through the ICDI")
    raise SystemExit(0)

print("Bytes arrived, but the banner marker %r was not found." % BANNER_MARKER)
raise SystemExit(1)
PYTHON
