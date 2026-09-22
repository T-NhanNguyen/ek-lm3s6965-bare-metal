#!/usr/bin/env bash
#
# Cable and connection check for the EK-LM3S6965.
# Run this while plugging/unplugging to see what the host actually detects.
# Read-only: touches no hardware state.
#
# NOTE: this deliberately parses `ioreg`, not `system_profiler`. On modern macOS,
# system_profiler SPUSBDataType does not report this board's ICDI at all, which
# produced a false "NOT FOUND" verdict in an earlier version of this script.

set -uo pipefail

EXPECTED_VENDOR_ID="0403"
EXPECTED_PRODUCT_ID="bcd9"
EXPECTED_PRODUCT_NAME="Stellaris Evaluation Board"

usb_dump_file=$(mktemp)
trap 'rm -f "$usb_dump_file"' EXIT
ioreg -p IOUSB -w0 -l 2>/dev/null > "$usb_dump_file"

probe_listing=$(python3 - "$usb_dump_file" <<'PYTHON'
import re
import sys

text = open(sys.argv[1]).read()

for block in text.split("+-o "):
    vendor = re.search(r'"idVendor" = (\d+)', block)
    product = re.search(r'"idProduct" = (\d+)', block)
    if not (vendor and product):
        continue

    name = re.search(r'"USB Product Name" = "([^"]+)"', block)
    serial = re.search(r'"USB Serial Number" = "([^"]+)"', block)

    print("{:04X}:{:04X}|{}|{}".format(
        int(vendor.group(1)),
        int(product.group(1)),
        name.group(1) if name else "?",
        serial.group(1) if serial else "(none)",
    ))
PYTHON
)

echo "=============================================="
echo " ICDI probe detection"
echo "=============================================="

if [[ -z "$probe_listing" ]]; then
    echo "  No USB devices reported at all -- unexpected."
    exit 1
fi

matched_probe=$(echo "$probe_listing" | grep -i "^${EXPECTED_VENDOR_ID}:${EXPECTED_PRODUCT_ID}" || true)

if [[ -n "$matched_probe" ]]; then
    echo "  FOUND  ${EXPECTED_VENDOR_ID}:${EXPECTED_PRODUCT_ID}"
    echo "         Name   : $(echo "$matched_probe" | cut -d'|' -f2)"
    echo "         Serial : $(echo "$matched_probe" | cut -d'|' -f3)"
else
    echo "  NOT FOUND -- expected ${EXPECTED_VENDOR_ID}:${EXPECTED_PRODUCT_ID} (${EXPECTED_PRODUCT_NAME})"
    echo
    echo "  USB devices currently present:"
    echo "$probe_listing" | while IFS='|' read -r identifiers name _serial; do
        echo "    ${identifiers}  ${name}"
    done
    echo
    echo "  Checks:"
    echo "    - Is the board powered? The ICDI draws power from USB."
    echo "    - Try a different cable -- many USB cables are charge-only."
    echo "    - Try a port directly on the machine, not through a hub."
fi

echo
echo "=============================================="
echo " Serial console (/dev)"
echo "=============================================="

serial_ports=$(ls /dev/cu.usbserial-* /dev/cu.usbmodem-* /dev/tty.usbserial-* 2>/dev/null || true)

if [[ -n "$serial_ports" ]]; then
    echo "$serial_ports" | sed 's/^/  /'
else
    echo "  none"
    echo
    echo "  The ICDI console on this board is one-way."
    echo
    echo "    1. Host to target: WORKS. Bytes written to the FT2232 channel B"
    echo "       arrive in the UART0 receive FIFO, over the net named VCP_RX."
    echo "    2. Target to host: does NOT come from PA1. The ICDI listens on a"
    echo "       different MCU pin, over the net named VCP_TX_SWO. So the banner"
    echo "       cannot reach the ICDI, and macOS cannot bind the FTDI VCP driver"
    echo "       to this board anyway (the extension whitelist omits"
    echo "       0x0403:0xbcd9)."
    echo
    echo "  Attach a USB serial adapter to the board's UART0 header for the full"
    echo "  two-way console:"
    echo "    PA1 (UART0 TX) -> adapter RXD"
    echo "    PA0 (UART0 RX) -> adapter TXD"
    echo "    GND            -> adapter GND"
    echo
    echo "  Then run: scripts/console.sh"
    echo
    echo "  This does NOT block flashing -- OpenOCD talks raw USB via libusb."
fi

echo
echo "  All /dev/cu.* for reference:"
ls /dev/cu.* 2>/dev/null | sed 's/^/    /'

echo
echo "=============================================="
echo " Verdict"
echo "=============================================="
if [[ -n "$matched_probe" ]]; then
    echo "  Probe present -- scripts/probe.sh and scripts/flash.sh will work."
else
    echo "  No probe. Fix cabling/power before running scripts/probe.sh"
fi
