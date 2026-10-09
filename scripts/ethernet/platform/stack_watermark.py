#!/usr/bin/env python3
"""Offline FTP stack written-watermark scanner; never accesses a device."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys

STACK_LIMIT = 0x2000E000
ESTACK = 0x20010000
SENTINEL = 0xA5C39E71
# Explicit target little-endian word order, independent of the host ABI.
SENTINEL_BYTES = SENTINEL.to_bytes(4, byteorder="little")


@dataclass(frozen=True)
class Watermark:
    first_mismatch: int | None
    written_watermark_bytes: int
    untouched_prefix_bytes: int


def scan(data: bytes, start: int = STACK_LIMIT, end: int = ESTACK) -> Watermark:
    """Fail closed on anything except this image's exact 8 KiB stack dump.

    Compare bytes ascending, not a count of changed words or a downward scan:
    sentinel holes above the first mismatch must not shorten the watermark.
    Unwritten frames and sentinel collisions can hide deeper historical SP.
    """
    if start != STACK_LIMIT or end != ESTACK:
        raise ValueError("bounds must be [0x2000e000, 0x20010000)")
    if len(data) != end - start:
        raise ValueError(f"dump must contain exactly {end - start} bytes; got {len(data)}")
    for offset, value in enumerate(data):
        if value != SENTINEL_BYTES[offset % 4]:
            return Watermark(start + offset, end - start - offset, offset)
    return Watermark(None, 0, end - start)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="raw binary dump (exactly 8192 bytes)")
    parser.add_argument("--start", type=lambda value: int(value, 0), default=STACK_LIMIT)
    parser.add_argument("--end", type=lambda value: int(value, 0), default=ESTACK)
    args = parser.parse_args(argv)
    try:
        result = scan(args.dump.read_bytes(), args.start, args.end)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    print(f"bounds: [0x{args.start:08x}, 0x{args.end:08x})")
    print(f"sentinel: 0x{SENTINEL:08x} (little-endian {SENTINEL_BYTES.hex(' ')})")
    mismatch = "none" if result.first_mismatch is None else f"0x{result.first_mismatch:08x}"
    print(f"first mismatch: {mismatch}")
    print(f"written watermark: {result.written_watermark_bytes} bytes")
    print(f"untouched prefix: {result.untouched_prefix_bytes} bytes (not a proven safe margin)")
    print("Not exact peak/minimum MSP: includes startup/idle; unwritten slots, sentinel")
    print("collisions and unexercised paths can hide deeper use. Boundary hit does not prove overflow.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
