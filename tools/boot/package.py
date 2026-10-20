#!/usr/bin/env python3
"""Offline v1 signed packager. See docs/boot-package.md for the input contract."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASES = {"A": 0x8800, "B": 0x24000}
MIN_PAYLOAD, MAX_PAYLOAD = 236, 111616
MAX_INPUT = 16 * 1024 * 1024
ORDER = int("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", 16)
SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


class PackageError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PackageError(message)


def span(start, size, bound, label):
    require(0 <= start <= bound and 0 <= size <= bound - start, label)


def read_input(path):
    require(stat.S_ISREG(path.stat().st_mode), "input must be a regular file")
    with path.open("rb") as stream:
        require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), "input must be a regular file")
        data = stream.read(MAX_INPUT + 1)
    require(len(data) <= MAX_INPUT, "input exceeds 16 MiB limit")
    return data


def canonical_payload(data, slot, input_format):
    base = BASES[slot]
    payload = elf_payload(data, base) if input_format == "elf" else data
    require(MIN_PAYLOAD <= len(payload) <= MAX_PAYLOAD, "payload length outside slot limits")
    payload += b"\xff" * (-len(payload) % 4)
    require(len(payload) <= MAX_PAYLOAD, "padded payload outside slot")
    msp, reset = struct.unpack_from("<II", payload)
    require(msp == 0x20010000, "initial MSP must equal 0x20010000")
    require(reset & 1, "reset vector must be Thumb")
    require(base <= (reset & ~1) < base + len(payload), "reset target outside authenticated payload")
    return payload


def elf_payload(data, base):
    """Bounded ELF32 parser; PT_LOAD p_paddr is the explicit flash LMA contract."""
    require(len(data) >= 52, "truncated ELF header")
    ident = data[:16]
    require(ident[:7] == b"\x7fELF\x01\x01\x01" and ident[7] == 0 and ident[8:] == bytes(8),
            "requires ELF32 little-endian System V version 1")
    (kind, machine, version, entry, phoff, shoff, flags, ehsize,
     phsize, phnum, shsize, shnum, shstr) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    require(kind == 2 and machine == 40 and version == 1 and ehsize == 52,
            "requires ARM executable ELF")
    require(flags & 0xff000000 == 0x05000000 and not flags & ~0x05000200,
            "requires ARM EABI5 soft-float (no unsupported flags)")
    require(phsize == 32 and 1 <= phnum <= 128, "unsupported program header table")
    require(shsize == 40 and 1 <= shnum <= 4096 and 0 < shstr < shnum,
            "requires ordinary section table (no extended numbering)")
    span(phoff, phnum * phsize, len(data), "truncated program headers")
    span(shoff, shnum * shsize, len(data), "truncated section headers")
    require(phoff >= 52 and shoff >= 52, "header table overlaps ELF header")
    require(phoff + phnum * 32 <= shoff or shoff + shnum * 40 <= phoff,
            "overlapping header tables")
    loads, flash = [], []
    for i in range(phnum):
        typ, off, vma, lma, filesz, memsz, perms, align = struct.unpack_from("<8I", data, phoff + i * 32)
        require(typ in (0, 1, 4, 0x6474e551, 0x70000001), "unsupported program header type")
        span(off, filesz, len(data), "truncated segment")
        if typ != 1:
            continue
        require(filesz <= memsz and memsz > 0 and perms & ~7 == 0, "invalid load sizes/flags")
        span(vma, memsz, 1 << 32, "VMA overflow")
        span(lma, memsz, 1 << 32, "LMA overflow")
        require(align in (0, 1) or (align & (align - 1) == 0 and vma % align == off % align),
                "invalid load alignment")
        in_sram = 0x20000000 <= vma and vma + memsz <= 0x20010000
        in_flash = base <= vma and vma + memsz <= base + MAX_PAYLOAD
        require(in_sram or in_flash, "load VMA outside selected slot/SRAM")
        if in_flash:
            require(vma == lma and filesz == memsz, "flash VMA/LMA mismatch or zero-fill flash")
        if filesz:
            require(base <= lma and lma + filesz <= base + MAX_PAYLOAD,
                    "file-backed load outside selected flash payload")
            require(off >= 52, "load includes ELF header")
            require(not (off < phoff + phnum * 32 and phoff < off + filesz) and
                    not (off < shoff + shnum * 40 and shoff < off + filesz), "load overlaps header table")
            flash.append((lma, data[off:off + filesz]))
        else:
            require(in_sram and (0x20000000 <= lma <= 0x20010000 or base <= lma <= base + MAX_PAYLOAD),
                    "unsupported nonloaded segment")
        for old in loads:
            require(vma + memsz <= old[0] or old[1] <= vma,
                    "overlapping load VMAs")
            if filesz and old[3]:
                require(off + filesz <= old[2] or old[2] + old[3] <= off, "overlapping load file spans")
        loads.append((vma, vma + memsz, off, filesz, memsz, perms))
    require(flash, "ELF has no flash loads")
    flash.sort()
    require(flash[0][0] == base, "ELF not linked at selected vector origin")
    end = base
    for address, content in flash:
        require(address >= end, "overlapping flash LMAs")
        end = address + len(content)
    require(any(v <= (entry & ~1) < v + fs and perms & 1
                for v, _, _, fs, _, perms in loads), "ELF entry not in executable load")
    # Ordinary numbering only: section zero is the canonical all-zero SHT_NULL
    # sentinel, not an extended-numbering carrier (sh_size/link/info).
    require(data[shoff:shoff + 40] == bytes(40), "requires canonical SHT_NULL section zero")
    names = struct.unpack_from("<10I", data, shoff + shstr * 40)
    require(names[1] == 3 and names[5] > 0, "requires nonempty SHT_STRTAB section names")
    span(names[4], names[5], len(data), "truncated section names")
    name_table = data[names[4]:names[4] + names[5]]
    require(name_table[0] == 0 and name_table[-1] == 0, "invalid section name string table")
    # Cross-check every allocated section so malformed/omitted load bytes cannot
    # silently disappear. Section names are not authority for data or bss.
    allocated = []
    for i in range(1, shnum):
        name, typ, attrs, addr, off, size, link, info, align, entsize = struct.unpack_from("<10I", data, shoff + i * 40)
        require(name < len(name_table) and name_table.find(b"\0", name) != -1,
                "invalid section name reference")
        require(not attrs & 0x800, "compressed sections unsupported")
        # gABI sh_addralign constrains sh_addr, not sh_offset. PT_LOAD's
        # p_vaddr/p_offset congruence and section/load mapping are checked above
        # and below; NOBITS sh_offset is conceptual, not a file-byte address.
        require(align in (0, 1) or (align & (align - 1) == 0 and addr % align == 0),
                "invalid section alignment")
        if typ != 8:
            span(off, size, len(data), "truncated section")
        require(not attrs & 0x400, "TLS sections unsupported")
        if not attrs & 2 or not size:
            continue
        require(typ in (1, 8, 14, 15, 16, 0x70000001), "unsupported allocated section")
        span(addr, size, 1 << 32, "section address overflow")
        for start, end_addr in allocated:
            require(addr + size <= start or end_addr <= addr, "overlapping allocated sections")
        allocated.append((addr, addr + size))
        matches = [load for load in loads if load[0] <= addr and addr + size <= load[1]]
        require(len(matches) == 1, "allocated section missing unique load mapping")
        vma, _, seg_off, filesz, memsz, perms = matches[0]
        delta = addr - vma
        if typ == 8:
            require(0x20000000 <= addr and addr + size <= 0x20010000 and delta >= filesz,
                    "NOBITS must be nonloaded SRAM")
        else:
            require(delta + size <= filesz and off == seg_off + delta,
                    "section/load file mapping conflict")
    payload = bytearray(b"\xff" * (end - base))
    for address, content in flash:
        payload[address - base:address - base + len(content)] = content
    return bytes(payload)


def der_to_raw(der):
    require(8 <= len(der) <= 72 and der[0] == 0x30 and der[1] == len(der) - 2,
            "invalid DER signature sequence")
    pos, scalars = 2, []
    for _ in range(2):
        require(pos + 2 <= len(der) and der[pos] == 2, "invalid DER integer tag")
        size = der[pos + 1]
        pos += 2
        encoded = der[pos:pos + size]
        require(1 <= size <= 33 and len(encoded) == size and not encoded[0] & 0x80,
                "invalid DER integer length/sign")
        require(size == 1 or encoded[0] != 0 or encoded[1] & 0x80, "nonminimal DER integer")
        value = int.from_bytes(encoded, "big")
        require(1 <= value < ORDER, "signature scalar outside P-256 range")
        scalars.append(value.to_bytes(32, "big"))
        pos += size
    require(pos == len(der), "trailing DER signature bytes")
    return b"".join(scalars)


def openssl_run(args, data=None):
    # Never forward tool stderr: private paths/diagnostic contents are not logs.
    try:
        result = subprocess.run(["openssl", *args], input=data, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise PackageError("unable to execute OpenSSL") from exc
    require(result.returncode == 0, "OpenSSL operation failed")
    return result.stdout


def trusted_public():
    header = (ROOT / "config/boot_demo_public_key.h").read_text()
    initializer = header.split("= {", 1)[1].split("};", 1)[0]
    public = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", initializer))
    require(len(public) == 65 and public[0] == 4, "invalid fixed demo public header")
    return public


def validate_public(public):
    """Validate a raw uncompressed SEC1 P-256 point, without writing headers."""
    require(len(public) == 65 and public[0] == 4,
            "public key must be exactly 65-byte uncompressed SEC1")
    prime = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
    b = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
    x, y = int.from_bytes(public[1:33], "big"), int.from_bytes(public[33:], "big")
    require(x < prime and y < prime, "public key coordinate outside P-256 field")
    require((y * y - (x * x * x - 3 * x + b)) % prime == 0,
            "public key point is not on P-256")
    return public


def validate_key(key, public_key=None):
    require(key.is_absolute(), "private key path must be absolute and external")
    resolved = key.resolve(strict=True)
    require(not resolved.is_relative_to(ROOT), "private key must remain outside repository")
    require(not key.is_symlink() and stat.S_ISREG(resolved.stat().st_mode), "key must be a regular nonsymlink file")
    require(stat.S_IMODE(resolved.stat().st_mode) == 0o600 and
            stat.S_IMODE(resolved.parent.stat().st_mode) == 0o700, "key/directory require modes 0600/0700")
    if public_key is None:
        public = validate_public(trusted_public())
    else:
        require(not public_key.samefile(resolved), "public key cannot alias private key")
        require(stat.S_ISREG(public_key.stat().st_mode), "public key must be a regular file")
        with public_key.open("rb") as stream:
            require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), "public key must be a regular file")
            public = validate_public(stream.read(66))
    openssl_run(["pkey", "-in", str(resolved), "-check", "-noout", "-passin", "pass:"])
    public_der = openssl_run(["pkey", "-in", str(resolved), "-pubout", "-outform", "DER",
                             "-ec_conv_form", "uncompressed", "-passin", "pass:"])
    require(len(public_der) == 91 and public_der[:26] == SPKI_PREFIX,
            "key must be named-curve P-256")
    require(public_der[26:] == public, "key does not match selected trust root")
    return resolved


def build_package(payload, slot, release, key):
    require(0 <= release <= 0xffffffff, "release must be uint32")
    prefix = (b"LM3S6965-FW-V1\0\0" + struct.pack("<8I", 1, 160, 1,
              0 if slot == "A" else 1, BASES[slot], len(payload), release, 0) +
              hashlib.sha256(payload).digest() + bytes(16))
    digest = hashlib.sha256(prefix).digest()
    signature = openssl_run(["pkeyutl", "-sign", "-inkey", str(key), "-passin", "pass:",
                             "-pkeyopt", "digest:sha256"], digest)
    return prefix + der_to_raw(signature) + payload


def publish(output, content):
    # Hard-link an fsynced same-directory temporary file: atomic publication,
    # exclusive destination creation (including dangling symlinks), no replace.
    temp = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".boot-package-", dir=output.parent, delete=False) as stream:
            temp = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temp, output)
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)


def package(input_path, output, slot, release, key, input_format, public_key=None):
    require(not os.path.lexists(output), "output already exists (no overwrite)")
    require(output.parent.is_dir(), "output parent must exist")
    resolved_key = validate_key(key, public_key)
    require(input_path.resolve() != resolved_key and not input_path.samefile(resolved_key),
            "input cannot be private key (including aliases)")
    payload = canonical_payload(read_input(input_path), slot, input_format)
    content = build_package(payload, slot, release, resolved_key)
    publish(output, content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--input-format", required=True, choices=("elf", "bin"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--slot", required=True, choices=("A", "B"))
    parser.add_argument("--release", required=True, type=int)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--public-key", type=Path,
                        help="raw 65-byte SEC1 P-256 trust root (default: fixed demo header)")
    args = parser.parse_args()
    try:
        require(0 <= args.release <= 0xffffffff, "release must be uint32")
        package(args.input, args.output, args.slot, args.release, args.private_key,
                args.input_format, args.public_key)
    except (PackageError, OSError) as exc:
        # OSError text may contain a sensitive path. Keep failures path-free.
        print("package: " + (str(exc) if isinstance(exc, PackageError) else "filesystem operation failed"), file=sys.stderr)
        return 1
    print("package: signed artifact created")
    return 0


if __name__ == "__main__":
    sys.exit(main())
