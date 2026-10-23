"""Bounded, test-only ELF32 resource gate; never executes target code."""
import argparse
import json
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect(data, *, execution=False, ftp=False, old_ftp_2k=False):
    require(52 <= len(data) <= 16 * 1024 * 1024, "ELF size outside probe bound")
    require(data[:7] == b"\x7fELF\x01\x01\x01", "requires little-endian ELF32")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    typ, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = header
    require((typ, machine, version, ehsize, phsize, shsize) == (2, 40, 1, 52, 32, 40),
            "requires ARM executable with standard tables")
    require(0 < phnum <= 64 and 0 < shnum <= 4096 and shstr < shnum, "invalid table counts")

    def span(off, size):
        require(0 <= off <= len(data) and 0 <= size <= len(data) - off, "truncated ELF span")
        return data[off:off + size]

    require(phoff >= 52 and shoff >= 52, "table overlaps ELF header")
    span(phoff, phnum * phsize)
    span(shoff, shnum * shsize)
    headers = [struct.unpack_from("<10I", data, shoff + i * 40) for i in range(shnum)]
    require(headers[shstr][1] == 3, "invalid section string table")
    names = span(headers[shstr][4], headers[shstr][5])

    def name(table, index):
        require(index < len(table), "string index outside table")
        end = table.find(b"\0", index)
        require(end >= 0, "unterminated ELF string")
        try:
            return table[index:end].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("non-ASCII ELF name") from error

    sections, symbols = {}, {}
    for h in headers[1:]:
        n = name(names, h[0])
        require(n not in sections, "duplicate section")
        sections[n] = h
        if h[1] != 8:
            span(h[4], h[5])
        if h[1] == 2:
            require(h[9] == 16 and h[5] % 16 == 0 and h[6] < shnum, "invalid symbol table")
            strings = headers[h[6]]
            require(strings[1] == 3, "invalid symbol strings")
            strings = span(strings[4], strings[5])
            for off in range(h[4], h[4] + h[5], 16):
                index, value, size, info, other, section = struct.unpack_from("<IIIBBH", data, off)
                n = name(strings, index)
                require(not n or section != 0, "undefined symbol: " + n)
                if n:
                    symbols[n] = (value, size, section)
    loads = [struct.unpack_from("<8I", data, phoff + i * 32) for i in range(phnum)]
    loads = [p for p in loads if p[0] == 1]
    require(loads, "no PT_LOAD")
    flash_end = 0
    for _, off, vma, lma, filesz, memsz, perms, align in loads:
        require(filesz <= memsz and vma + memsz <= 2**32, "invalid load size")
        span(off, filesz)
        require(align == 256 and vma % align == off % align, "invalid load alignment")
        if filesz:
            require(lma < 0x8000 and lma + filesz <= 0x8000, "flash load outside boot partition")
            flash_end = max(flash_end, lma + filesz)
            require(vma == lma or 0x20000000 <= vma <= vma + memsz <= 0x2000dfc0,
                    "loaded SRAM overlaps reservations")
        else:
            require(0x20000000 <= vma <= vma + memsz <= 0x20010000, "NOLOAD outside SRAM")
    for n, h in sections.items():
        if not h[2] & 2 or not h[5]:
            continue
        matching = [p for p in loads if p[2] <= h[3] and h[3] + h[5] <= p[2] + p[5]]
        require(len(matching) == 1, "allocated section lacks unique load: " + n)
        p = matching[0]
        delta = h[3] - p[2]
        require(delta >= p[4] if h[1] == 8 else
                delta + h[5] <= p[4] and h[4] == p[1] + delta, "section backing mismatch: " + n)
    def sym(n):
        require(n in symbols, "missing symbol: " + n)
        return symbols[n][0]
    require(not old_ftp_2k or ftp, "old capacity fixture must use FTP allocator")
    heap_end = sym("_heap_start") + (2048 if old_ftp_2k else 8192) if ftp else 0x2000dbc0
    for n, value in {"_flash_payload_start": 0, "_flash_payload_limit": 0x40000 if ftp else 0x8000,
                     "_diagnostic_start": 0x2000dfc0, "_diagnostic_end": 0x2000e000,
                     "_stack_limit": 0x2000e000, "_estack": 0x20010000,
                     "_heap_end": heap_end}.items():
        require(sym(n) == value, "wrong bound: " + n)
    require(sym("_flash_payload_end") == flash_end, "symbol/PT_LOAD flash end mismatch")
    require(sym("_sdata") == 0x20000000 <= sym("_edata") <= sym("_sbss") <= sym("_ebss")
            <= sym("_heap_start") <= sym("_heap_static_end") <= sym("_heap_end"), "invalid lower SRAM bounds")
    for n, addr, size in ((".boot_diagnostic", 0x2000dfc0, 64), (".stack", 0x2000e000, 8192)):
        require(n in sections and (sections[n][1], sections[n][3], sections[n][5]) == (8, addr, size),
                "wrong NOLOAD reservation: " + n)
    if not ftp:
        require(sym("_continuity_start") == 0x2000dbc0 and sym("_continuity_end") == 0x2000dfc0,
                "wrong continuity bounds")
        h = sections.get(".boot_continuity")
        require(h and (h[1], h[3], h[5]) == (8, 0x2000dbc0, 1024), "wrong continuity NOLOAD")
    vector = sections.get(".isr_vector")
    require(vector and vector[3] == 0 and vector[5] == 236, "wrong vectors")
    msp, reset = struct.unpack_from("<II", data, vector[4])
    reset_symbol = "FTP_Stack_Reset_Handler" if execution else "Reset_Handler"
    require(msp == 0x20010000 and reset == (sym(reset_symbol) | 1)
            and entry == (sym("Reset_Handler") | 1), "wrong reset/MSP")
    if execution:
        for n in ("__wrap_calloc", "__wrap_free", "__wrap__sbrk", "stack_depth", "slots"):
            require(n in symbols and symbols[n][1] > 0, "missing instrumentation: " + n)
    if ftp:
        require(sym("_heap_start") == sym("_heap_static_end"), "unexpected FTP static heap input")
        require("unused_generic_sbrk" not in symbols, "generic allocator retained in FTP fixture")
    for n in ("boot_verify_stored", "mbedtls_ecdsa_verify", "mbedtls_sha256_starts",
              "mbedtls_sha256_update", "mbedtls_sha256_finish", "calloc", "free", "_sbrk",
              "_calloc_r", "_free_r", "_malloc_r", "_sbrk_r", "main"):
        require(n in symbols and symbols[n][1] > 0, "missing retained code: " + n)
    key = symbols.get("lm3s6965_boot_demo_public_key")
    require(key and key[1] == 65 and key[0] < flash_end and 0 < key[2] < shnum,
            "missing 65-byte public key")
    key_section = headers[key[2]]
    require(key_section[1] == 1 and key_section[3] <= key[0] and
            key[0] + 65 <= key_section[3] + key_section[5], "public key not file backed")
    require(span(key_section[4] + key[0] - key_section[3], 65)[0] == 4,
            "public key not uncompressed SEC1")
    require(not any(n.startswith(("mbedtls_ecdsa_sign", "mbedtls_ecdsa_write_signature",
                                 "mbedtls_asn1_write")) for n in symbols), "unused signing retained")
    require(not sections.get(".unsupported_allocated", (0, 0, 0, 0, 0, 0))[5], "unknown allocated content")
    return {"flash_end": flash_end, "flash_margin": 0x8000 - flash_end,
            "data": sections[".data"][5], "bss": sections[".bss"][5],
            "heap_base": sym("_heap_static_end"), "heap_capacity": sym("_heap_end") - sym("_heap_static_end"),
            "loads": loads}


def check_map(text, *, ftp=False):
    require("Discarded input sections" in text and "Linker script and memory map" in text,
            "missing map structure")
    discarded, retained = text.split("Linker script and memory map", 1)
    require(".text.mbedtls_ecdsa_sign" in discarded, "no discarded signing evidence")
    for section, owner in ((".text._sbrk", "ftp_sbrk.c.obj" if ftp else "syscalls.c.obj"),
                           (".text.calloc", "libc.a(libc_a-calloc.o)"),
                           (".text.free", "libc.a(libc_a-malloc.o)"),
                           (".text._calloc_r", "libc.a(libc_a-callocr.o)"),
                           (".text._malloc_r", "libc.a(libc_a-mallocr.o)"),
                           (".text._free_r", "libc.a(libc_a-freer.o)"),
                           (".text._sbrk_r", "libc.a(libc_a-sbrkr.o)")):
        require(re.search(re.escape(section) + r"\s+0x[0-9a-f]+\s+0x[0-9a-f]+[^\n]*" + re.escape(owner), retained),
                "wrong production allocator owner")
    for n in ("libc_a-calloc.o", "libc_a-malloc.o", "libc_a-mallocr.o", "libc_a-freer.o", "libc_a-sbrkr.o"):
        require("libc.a(" + n + ")" in retained, "missing full newlib input: " + n)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--map", required=True, type=Path)
    args = parser.parse_args()
    report = inspect(args.elf.read_bytes())
    check_map(args.map.read_text())
    print(json.dumps(report, indent=2))
