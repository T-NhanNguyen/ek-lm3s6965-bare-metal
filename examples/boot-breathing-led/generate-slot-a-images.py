#!/usr/bin/env python3
"""Generate this fixture's unsigned slot-A bin/hex from nonempty flash loads."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.boot.package import BASES, MAX_PAYLOAD, PackageError, elf_payload, read_input, require


def flash_sections(data):
    # Reuse the bounded ELF validator: rejects file-backed loads outside slot A,
    # ambiguous section/load mappings, malformed tables and non-SRAM NOBITS.
    # SRAM VMA with FLASH LMA (.data) is valid; RAM NOLOAD is not image content.
    elf_payload(data, BASES["A"])
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phnum = struct.unpack_from("<H", data, 44)[0]
    shnum, shstr = struct.unpack_from("<HH", data, 48)
    loads = []
    for i in range(phnum):
        typ, off, vma, lma, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * 32)
        if typ == 1 and filesz:
            loads.append((off, vma, lma, filesz))
    headers = [struct.unpack_from("<10I", data, shoff + i * 40) for i in range(shnum)]
    names_header = headers[shstr]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    section_names = [names[h[0]:names.index(b"\0", h[0])] for h in headers]
    selected = []
    for header, name in zip(headers, section_names):
        _, typ, attrs, addr, off, size, *_ = header
        if not attrs & 2 or typ == 8 or not size:
            continue
        matches = [(lma + addr - vma) for seg_off, vma, lma, filesz in loads
                   if 0 <= addr - vma and addr - vma + size <= filesz
                   and off == seg_off + addr - vma]
        require(len(matches) == 1, "image section missing unique file-backed load")
        lma = matches[0]
        require(BASES["A"] <= lma and lma + size <= BASES["A"] + MAX_PAYLOAD,
                "image section LMA outside slot-A payload")
        # objcopy section selectors are patterns, not indices. Reject ambiguous
        # names (including duplicates shared with excluded/empty sections).
        require(re.fullmatch(rb"[A-Za-z0-9_.]+", name) is not None
                and section_names.count(name) == 1, "unsafe or duplicate image section name")
        selected.append("--only-section=" + name.decode("ascii"))
    require(selected, "no nonempty allocated flash sections")
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--hex", required=True, type=Path)
    args = parser.parse_args()
    try:
        sections = flash_sections(read_input(args.elf))
        for fmt, output in (("binary", args.bin), ("ihex", args.hex)):
            subprocess.run([args.objcopy, "--gap-fill=0xff", *sections,
                            "-O", fmt, str(args.elf), str(output)], check=True)
    except (PackageError, OSError, subprocess.CalledProcessError) as exc:
        print("slot-A images: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
