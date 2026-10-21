#!/usr/bin/env bash
#
# Flash one A/B boot slot on an EK-LM3S6965 over the on-board ICDI probe.
#
# Usage:
#   scripts/ab-boot-firmwareflash-openocd.sh <A|B> <firmware-pkg>
#   scripts/ab-boot-firmwareflash-openocd.sh --help
#
# The package is a transport artifact: exactly 160 manifest bytes followed by
# the exact canonical payload (see docs/boot-package.md). This script validates
# the package fully BEFORE touching flash, erases only the selected slot's
# payload pages plus its manifest page, writes payload + manifest, reads both
# back for byte comparison, and only then resets into the new image.
#
# Flash geometry (see docs/boot-memory-layout.md, include/lm3s6965/boot_format.h):
#   slot A payload   [0x8800, 0x23C00), manifest at 0x23C00 (sector 143)
#   slot B payload  [0x24000, 0x3F400), manifest at 0x3F400 (sector 253)
#   sector number = byte address / 1024. Erase ranges are page-rounded and stay
#   inside the selected slot: boot [0, 0x8000), metadata [0x8000, 0x8800),
#   the opposite slot, and spare [0x3F800, 0x40000) are never erased.
#
# OpenOCD sequence (single halted session, then a separate reset only on match):
#   halt -> erase payload pages + manifest page -> write payload + manifest ->
#   dump readback -> host cmp + sha256 log (mismatch: stop, exit 1) ->
#   reset run (match only).
#
# Any package validation failure exits 1 before any erase or write.

set -euo pipefail

SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(dirname "$SCRIPT_DIRECTORY")
BOARD_CONFIG="${REPOSITORY_ROOT}/tools/openocd/board/ek-lm3s6965.cfg"

usage() {
    cat <<'EOF'
Usage:
  ab-boot-firmwareflash-openocd.sh <A|B> <firmware-pkg>
  ab-boot-firmwareflash-openocd.sh --help

Flash one A/B boot slot on an EK-LM3S6965 via OpenOCD and the on-board ICDI
probe, then reset into the new image.

Arguments:
  <A|B>          Slot selector. Must match the manifest's slot field, and
                 selects the payload base, bound, manifest address and erase
                 sectors below. Exactly "A" or "B".
  <firmware-pkg> Package file: exactly 160 manifest bytes followed by the
                 exact canonical payload (total 396..111776 bytes).

Geometry (sector = address / 1024, page size 1024):
  slot A payload [0x8800, 0x23C00),  manifest 0x23C00 (sector 143)
  slot B payload [0x24000, 0x3F400), manifest 0x3F400 (sector 253)
Boot [0, 0x8000), metadata [0x8000, 0x8800), the opposite slot and spare
[0x3F800, 0x40000) are never erased.

Validation (all failures exit 1 before any erase):
  magic "LM3S6965-FW-V1", format 1, size 160, product 1, flags 0 with 16
  zeroed reserved bytes; slot/base/length match the selected slot and its
  bound (length 236..111616, multiple of 4); total file length equals
  160 + payload length; SHA-256 over the whole payload matches the manifest;
  initial MSP is exactly 0x20010000; reset vector has the Thumb bit set and
  its cleared target lies in [base, base + length).

Sequence:
  halt, erase payload pages + manifest page, write payload and manifest,
  dump readback, host cmp + sha256 log (mismatch stops, exit 1), reset run
  only on a full match.

Exit codes:
  0  success (or --help)
  1  any usage, environment, validation, OpenOCD, or readback-mismatch failure
EOF
}

fail() {
    echo "error: $1" >&2
    exit 1
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

[[ $# -eq 2 ]] || fail "expected exactly 2 arguments: <A|B> <firmware-pkg>. See --help."
SLOT_LETTER="$1"
PKG="$2"

case "$SLOT_LETTER" in
    A) SLOT_NUM=0; PAYLOAD_BASE=0x8800; PAYLOAD_LIMIT=0x23C00; MANIFEST_ADDR=0x23C00 ;;
    B) SLOT_NUM=1; PAYLOAD_BASE=0x24000; PAYLOAD_LIMIT=0x3F400; MANIFEST_ADDR=0x3F400 ;;
    *) fail "slot must be exactly A or B, got: $SLOT_LETTER" ;;
esac

[[ -f "$PKG" ]] || fail "package not found: $PKG"
[[ -r "$PKG" ]] || fail "package not readable: $PKG"

command -v openocd >/dev/null 2>&1 || fail "openocd not found on PATH"
command -v python3 >/dev/null 2>&1 || fail "python3 not found on PATH (needed for package validation)"
[[ -f "$BOARD_CONFIG" ]] || fail "board config not found: $BOARD_CONFIG"

TMPDIR="$(mktemp -d "${TMPDIR:-/tmp}/ab-boot-flash.XXXXXX")"
trap 'rm -rf "$TMPDIR"' EXIT
PAYLOAD_BIN="$TMPDIR/payload.bin"
MANIFEST_BIN="$TMPDIR/manifest.bin"
READBACK_PAYLOAD="$TMPDIR/readback-payload.bin"
READBACK_MANIFEST="$TMPDIR/readback-manifest.bin"

# Full offline package validation + split into payload/manifest images.
# Prints validated numeric fields and Tcl-encoded filenames on stdout on
# success; diagnostics go to stderr. Exits 1 on failure. No flash is touched.
VALIDATED="$(python3 - "$PKG" "$SLOT_LETTER" "$SLOT_NUM" "$PAYLOAD_BIN" "$MANIFEST_BIN" "$READBACK_PAYLOAD" "$READBACK_MANIFEST" <<'PYEOF'
import hashlib
import struct
import sys

pkg_path, slot_letter, slot_num_s, payload_out, manifest_out = sys.argv[1:6]
slot_num = int(slot_num_s)
base = 0x8800 if slot_letter == "A" else 0x24000
limit = 0x23C00 if slot_letter == "A" else 0x3F400

with open(pkg_path, "rb") as f:
    data = f.read()

def fail(msg):
    print("error: package validation failed: %s" % msg, file=sys.stderr)
    sys.exit(1)

if len(data) < 160:
    fail("file shorter than 160-byte manifest (%d bytes)" % len(data))
manifest, payload = data[:160], data[160:]

if manifest[0:16] != b"LM3S6965-FW-V1\x00\x00":
    fail("bad magic")
fmt, size, product, slot, mbase, mlen, release, flags = struct.unpack("<8I", manifest[16:48])
if fmt != 1:
    fail("unsupported format %d (expected 1)" % fmt)
if size != 160:
    fail("bad manifest size field %d (expected 160)" % size)
if product != 1:
    fail("unexpected product id %d (expected 1)" % product)
if flags != 0:
    fail("nonzero flags field %d" % flags)
if manifest[80:96] != bytes(16):
    fail("nonzero reserved bytes")
if slot != slot_num:
    fail("manifest slot %d does not match selected slot %s" % (slot, slot_letter))
if mbase != base:
    fail("manifest base 0x%x does not match slot %s base 0x%x" % (mbase, slot_letter, base))
if len(payload) != mlen:
    fail("file payload %d bytes != manifest length %d" % (len(payload), mlen))
if mlen < 236 or mlen > (limit - base) or mlen % 4 != 0:
    fail("payload length %d out of bounds (236..%d, multiple of 4)" % (mlen, limit - base))
if base + mlen > limit:
    fail("payload end 0x%x exceeds slot bound 0x%x" % (base + mlen, limit))
if hashlib.sha256(payload).digest() != manifest[48:80]:
    fail("payload SHA-256 mismatch")
if len(payload) < 8:
    fail("payload too short for vectors")
msp, reset = struct.unpack("<2I", payload[0:8])
if msp != 0x20010000:
    fail("initial MSP 0x%08x != 0x20010000" % msp)
if reset & 1 == 0:
    fail("reset vector 0x%08x lacks Thumb bit" % reset)
if reset & ~1 < base or reset & ~1 >= base + mlen:
    fail("reset target 0x%08x outside [0x%x, 0x%x)" % (reset & ~1, base, base + mlen))

with open(payload_out, "wb") as f:
    f.write(payload)
with open(manifest_out, "wb") as f:
    f.write(manifest)

print("validated slot %s release %d payload %d bytes sha256=%s"
      % (slot_letter, release, mlen, hashlib.sha256(payload).hexdigest()), file=sys.stderr)
print("PAYLOAD_LEN=%d" % mlen)
print("RELEASE=%d" % release)


def tcl_word(path):
    # Shell quoting does not protect a word in an OpenOCD Tcl command string.
    # Use a quoted Tcl word, escaping substitutions and physical line breaks;
    # braces alone are unsafe for paths containing braces or backslash-newline.
    escapes = {"\\": "\\\\", '"': '\\"', "$": "\\$", "[": "\\[", "]": "\\]",
               "\n": "\\n", "\r": "\\r"}
    return '"' + "".join(escapes.get(char, char) for char in path) + '"'


for name, path in (("PAYLOAD_TCL", payload_out), ("MANIFEST_TCL", manifest_out),
                   ("READBACK_PAYLOAD_TCL", sys.argv[6]),
                   ("READBACK_MANIFEST_TCL", sys.argv[7])):
    print("%s=%s" % (name, tcl_word(path)))
PYEOF
)" || fail "package validation failed (no flash erased)"

PAYLOAD_LEN=""
PAYLOAD_TCL="" MANIFEST_TCL="" READBACK_PAYLOAD_TCL="" READBACK_MANIFEST_TCL=""
while IFS= read -r line; do
    case "$line" in
        PAYLOAD_LEN=*) PAYLOAD_LEN="${line#PAYLOAD_LEN=}" ;;
        PAYLOAD_TCL=*) PAYLOAD_TCL="${line#PAYLOAD_TCL=}" ;;
        MANIFEST_TCL=*) MANIFEST_TCL="${line#MANIFEST_TCL=}" ;;
        READBACK_PAYLOAD_TCL=*) READBACK_PAYLOAD_TCL="${line#READBACK_PAYLOAD_TCL=}" ;;
        READBACK_MANIFEST_TCL=*) READBACK_MANIFEST_TCL="${line#READBACK_MANIFEST_TCL=}" ;;
    esac
done <<< "$VALIDATED"
case "$PAYLOAD_LEN" in
    ''|*[!0-9]*) fail "internal error: bad validated payload length" ;;
esac

[[ -n "$PAYLOAD_TCL" && -n "$MANIFEST_TCL" && -n "$READBACK_PAYLOAD_TCL" && -n "$READBACK_MANIFEST_TCL" ]] ||
    fail "internal error: missing Tcl filename encoding"

# Page-rounded erase sectors, kept strictly inside the selected slot:
# payload pages [base, base + len) rounded up to 1024, plus the manifest page.
FIRST_SECTOR=$((PAYLOAD_BASE / 1024))
LAST_PAYLOAD_SECTOR=$(((PAYLOAD_BASE + PAYLOAD_LEN - 1) / 1024))
MANIFEST_SECTOR=$((MANIFEST_ADDR / 1024))
BASE_HEX="$(printf '0x%X' "$PAYLOAD_BASE")"
MANIFEST_HEX="$(printf '0x%X' "$MANIFEST_ADDR")"

echo "Flashing slot $SLOT_LETTER release from $PKG"
echo "  payload  $BASE_HEX + $PAYLOAD_LEN bytes (sectors $FIRST_SECTOR..$LAST_PAYLOAD_SECTOR)"
echo "  manifest $MANIFEST_HEX (sector $MANIFEST_SECTOR)"

# Halted session: erase, write, dump readback. Reset is a separate step below
# so a readback mismatch can stop before the new image ever runs.
openocd -f "$BOARD_CONFIG" \
    -c "init" \
    -c "halt" \
    -c "flash erase_sector 0 $FIRST_SECTOR $LAST_PAYLOAD_SECTOR" \
    -c "flash erase_sector 0 $MANIFEST_SECTOR $MANIFEST_SECTOR" \
    -c "flash write_bank 0 $PAYLOAD_TCL $BASE_HEX" \
    -c "flash write_bank 0 $MANIFEST_TCL $MANIFEST_HEX" \
    -c "dump_image $READBACK_PAYLOAD_TCL $BASE_HEX $PAYLOAD_LEN" \
    -c "dump_image $READBACK_MANIFEST_TCL $MANIFEST_HEX 160" \
    -c "shutdown" || fail "openocd erase/write/dump failed"

# Host-side readback check: exact byte comparison plus sha256 log.
python3 - "$PAYLOAD_BIN" "$MANIFEST_BIN" "$READBACK_PAYLOAD" "$READBACK_MANIFEST" <<'PYEOF' || exit 1
import hashlib
import sys

payload_bin, manifest_bin, rb_payload, rb_manifest = sys.argv[1:5]

def read(path):
    with open(path, "rb") as f:
        return f.read()

expected_payload, expected_manifest = read(payload_bin), read(manifest_bin)
got_payload, got_manifest = read(rb_payload), read(rb_manifest)

print("sha256 payload  expected %s" % hashlib.sha256(expected_payload).hexdigest())
print("sha256 payload  readback %s" % hashlib.sha256(got_payload).hexdigest())
print("sha256 manifest expected %s" % hashlib.sha256(expected_manifest).hexdigest())
print("sha256 manifest readback %s" % hashlib.sha256(got_manifest).hexdigest())

ok = True
if got_payload != expected_payload:
    print("error: payload readback mismatch", file=sys.stderr)
    ok = False
if got_manifest != expected_manifest:
    print("error: manifest readback mismatch", file=sys.stderr)
    ok = False
if not ok:
    sys.exit(1)
print("readback matches written images")
PYEOF

cmp "$PAYLOAD_BIN" "$READBACK_PAYLOAD" || fail "payload readback mismatch (stopping before reset)"
cmp "$MANIFEST_BIN" "$READBACK_MANIFEST" || fail "manifest readback mismatch (stopping before reset)"

# Full match only: reset into the new image.
openocd -f "$BOARD_CONFIG" \
    -c "init" \
    -c "reset run" \
    -c "shutdown" || fail "openocd reset failed after verified write"

echo "Slot $SLOT_LETTER flash verified; target reset."
