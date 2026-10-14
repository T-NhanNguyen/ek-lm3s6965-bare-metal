"""Standard-library tests; no private material is read by Python or copied."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("boot_package", ROOT / "tools/boot/package.py")
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)
KEY = Path.home() / ".local/share/lm3s6965-boot-demo" / ROOT.name / "signing-key.pem"
VERIFIER = None


def raw(slot="A", size=236):
    return struct.pack("<II", 0x20010000, p.BASES[slot] + 9) + bytes(size - 8)


def synthetic_elf(slot="A", size=236):
    """Independent minimal explicit ELF fixture, not the generator's encoder."""
    base = p.BASES[slot]
    payload = raw(slot, size)
    result = bytearray(0x100 + len(payload) + 80)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    result[:52] = struct.pack("<16sHHIIIIIHHHHHH", ident, 2, 40, 1, base + 9,
                             52, 0x100 + len(payload), 0x05000200, 52, 32, 1, 40, 2, 1)
    struct.pack_into("<8I", result, 52, 1, 0x100, base, base, size, size, 5, 0x100)
    result[0x100:0x100 + size] = payload
    # A present string table containing the canonical empty name.
    struct.pack_into("<10I", result, 0x100 + size + 40, 0, 3, 0, 0, 0xfc, 1, 0, 0, 1, 0)
    return bytes(result)


def command(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


class PayloadTests(unittest.TestCase):
    def test_raw_padding_limits_and_preservation(self):
        for slot in p.BASES:
            for size in (236, 237, 238, 239, 240, 111613, 111614, 111615, 111616):
                data = raw(slot, size)
                payload = p.canonical_payload(data, slot, "bin")
                self.assertEqual(payload[:size], data)
                self.assertEqual(payload[size:], b"\xff" * (-size % 4))
            for size in (235, 111617):
                with self.assertRaises(p.PackageError):
                    p.canonical_payload(raw(slot, size), slot, "bin")

    def test_vectors(self):
        for word, value in ((0, 0x2000fff8), (1, 0x8808), (1, 0x8801 + 236), (1, 1)):
            data = bytearray(raw())
            struct.pack_into("<I", data, word * 4, value)
            with self.assertRaises(p.PackageError):
                p.canonical_payload(data, "A", "bin")
        with self.assertRaises(p.PackageError):
            p.canonical_payload(raw("A"), "B", "bin")

    def test_elf_min_max_and_padding(self):
        for slot in p.BASES:
            for size in (236, 237, 111616):
                self.assertEqual(p.canonical_payload(synthetic_elf(slot, size), slot, "elf"),
                                 p.canonical_payload(raw(slot, size), slot, "bin"))

    def test_nonloaded_sram_and_overlaps(self):
        source = bytearray(synthetic_elf())
        struct.pack_into("<H", source, 44, 2)
        struct.pack_into("<8I", source, 84, 1, 0, 0x20001000, 0x20001000, 0, 32, 6, 1)
        self.assertEqual(p.canonical_payload(source, "A", "elf"), raw())
        # Overlapping VMA, overlapping file span, and duplicate flash LMA
        # each fail even if the overlapping bytes would be identical.
        for segment in ((1, 0, 0x8800, 0x8800, 0, 32, 5, 1),
                        (1, 0x100, 0x20001000, 0x8c00, 32, 32, 6, 1),
                        (1, 0x180, 0x20001000, 0x8800, 32, 32, 6, 1)):
            changed = bytearray(source)
            struct.pack_into("<8I", changed, 84, *segment)
            with self.assertRaises(p.PackageError):
                p.elf_payload(changed, 0x8800)

    def test_input_bounds_and_file_kind(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input"
            with path.open("wb") as stream:
                stream.truncate(p.MAX_INPUT + 1)
            with self.assertRaises(p.PackageError):
                p.read_input(path)
            fifo = Path(directory) / "fifo"
            os.mkfifo(fifo)
            with self.assertRaises(p.PackageError):
                p.read_input(fifo)

    def test_elf_malformed(self):
        source = synthetic_elf()
        for end in (0, 4, 51, 52, 83, len(source) - 1):
            with self.subTest(end=end), self.assertRaises(p.PackageError):
                p.elf_payload(source[:end], p.BASES["A"])
        # Header and PT_LOAD fields: wrong machine/type, extended numbering,
        # missing data, flash outside slot, overflow, zero-fill, bad alignment.
        mutations = [(4, "B", 2), (5, "B", 2), (16, "H", 3), (18, "H", 3),
                     (36, "I", 0x05000400), (44, "H", 129), (48, "H", 0),
                     (52, "I", 2), (56, "I", len(source)), (60, "I", 0),
                     (64, "I", 0x24000), (64, "I", 0xfffffff0),
                     (68, "I", 237), (72, "I", 237), (80, "I", 3)]
        for off, fmt, value in mutations:
            data = bytearray(source)
            struct.pack_into("<" + fmt, data, off, value)
            with self.subTest(off=off, value=value), self.assertRaises(p.PackageError):
                p.elf_payload(data, p.BASES["A"])
        with self.assertRaises(p.PackageError):
            p.elf_payload(source, p.BASES["B"])

    def test_der_strictness(self):
        self.assertEqual(p.der_to_raw(bytes.fromhex("3006020101020102")),
                         (1).to_bytes(32, "big") + (2).to_bytes(32, "big"))
        for der in (b"", bytes.fromhex("3006020100020102"), bytes.fromhex("3006020180020102"),
                    bytes.fromhex("300702020001020102"), bytes.fromhex("308106020101020102"),
                    bytes.fromhex("3006020101020102") + b"\0",
                    b"\x30\x26\x02\x21\0" + p.ORDER.to_bytes(32, "big") + b"\x02\x01\x01"):
            with self.subTest(der=der.hex()), self.assertRaises(p.PackageError):
                p.der_to_raw(der)

    def test_atomic_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            dest = Path(directory) / "output"
            p.publish(dest, b"complete")
            with self.assertRaises(FileExistsError):
                p.publish(dest, b"replace")
            self.assertEqual(dest.read_bytes(), b"complete")
            self.assertEqual(sorted(x.name for x in dest.parent.iterdir()), ["output"])
            link = dest.parent / "dangling"
            link.symlink_to(dest.parent / "missing")
            with self.assertRaises(FileExistsError):
                p.publish(link, b"replace")
            with patch.object(p.os, "link", side_effect=OSError("simulated")):
                with self.assertRaises(OSError):
                    p.publish(dest.parent / "failure", b"complete")
            self.assertFalse((dest.parent / "failure").exists())
            self.assertFalse(list(dest.parent.glob(".boot-package-*")))


def fixture_compiler():
    # Prefer the official bundle, with an explicit override for portability.
    candidates = sorted((Path.home() / ".toolchains").glob(
        "arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc"))
    candidates += sorted(Path("/Applications/ArmGNUToolchain").glob(
        "*/arm-none-eabi/bin/arm-none-eabi-gcc"))
    return os.environ.get("BOOT_ARM_GCC") or (str(candidates[-1]) if candidates else
                                             shutil.which("arm-none-eabi-gcc"))


class LinkedElfTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gcc = fixture_compiler()
        if not cls.gcc:
            raise unittest.SkipTest("arm-none-eabi-gcc unavailable: real linker fixtures not exercised")
        print("ARM fixture compiler: " + cls.gcc, flush=True)
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.elfs = {}
        assembly = cls.directory / "fixture.S"
        assembly.write_text(''' .syntax unified
 .thumb
 .section .vectors,"a",%progbits
 .word 0x20010000
 .word reset
 .space 228,0
 .section .text,"ax",%progbits
 .thumb_func
 .global reset
reset: b reset
 .section .data,"aw",%progbits
 .byte 0x12,0x34,0x56,0x78,0x9a
 .section .bss,"aw",%nobits
 .space 37
''')
        for slot, base in p.BASES.items():
            linker = cls.directory / (slot + ".ld")
            linker.write_text(f'''ENTRY(reset)
PHDRS {{ text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }}
SECTIONS {{
 . = {base};
 .vectors : {{ *(.vectors) }} :text
 .text : {{ *(.text) }} :text
 .data 0x20000000 : AT({base}+0x400) {{ *(.data) }} :data
 .bss (NOLOAD) : {{ *(.bss) }} :data
}}''')
            elf = cls.directory / (slot + ".elf")
            result = command([cls.gcc, "-mcpu=cortex-m3", "-mthumb", "-nostdlib", "-Wl,--build-id=none",
                              "-T", str(linker), str(assembly), "-o", str(elf)])
            if result.returncode:
                raise RuntimeError(result.stderr.decode())
            cls.elfs[slot] = elf

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_linker_load_semantics(self):
        for slot, elf in self.elfs.items():
            data = elf.read_bytes()
            payload = p.canonical_payload(data, slot, "elf")
            self.assertEqual(len(payload), 0x408)
            phoff = struct.unpack_from("<I", data, 28)[0]
            text_size = struct.unpack_from("<I", data, phoff + 16)[0]
            self.assertGreater(text_size, 236)
            self.assertEqual(payload[text_size:0x400], b"\xff" * (0x400 - text_size))
            self.assertEqual(payload[0x400:], bytes.fromhex("123456789affffff"))
            objcopy = str(Path(self.gcc).with_name("arm-none-eabi-objcopy"))
            self.assertIsNotNone(objcopy)
            binary = self.directory / (slot + ".bin")
            result = command([objcopy, "-O", "binary", "--gap-fill", "0xff", str(elf), str(binary)])
            self.assertEqual(result.returncode, 0)
            self.assertEqual(p.canonical_payload(binary.read_bytes(), slot, "bin"), payload)

    def test_real_elf_conflicts_and_sections(self):
        source = self.elfs["A"].read_bytes()
        phoff = struct.unpack_from("<I", source, 28)[0]
        shoff = struct.unpack_from("<I", source, 32)[0]
        shnum = struct.unpack_from("<H", source, 48)[0]
        # Data flash LMA overlap, wrong-slot data LMA, SRAM-only initialized
        # load, data VMA overlap, truncated data, and missing mapped section.
        mutations = [(phoff + 32 + 12, 0x8800), (phoff + 32 + 12, 0x24000),
                     (phoff + 32 + 12, 0x20000000), (phoff + 32 + 8, 0x8800),
                     (phoff + 32 + 4, len(source)), (phoff + 32 + 20, 0xffffffff)]
        for i in range(shnum):
            typ, attrs = struct.unpack_from("<II", source, shoff + i * 40 + 4)
            if typ == 1 and attrs & 2:
                mutations.append((shoff + i * 40 + 16, 0))
        for off, value in mutations:
            data = bytearray(source)
            struct.pack_into("<I", data, off, value)
            with self.subTest(off=off, value=value), self.assertRaises(p.PackageError):
                p.elf_payload(data, 0x8800)

    def test_section_structure_review_regressions(self):
        for slot, elf in self.elfs.items():
            source = elf.read_bytes()
            shoff = struct.unpack_from("<I", source, 32)[0]
            shnum, shstr = struct.unpack_from("<HH", source, 48)
            sections = [struct.unpack_from("<10I", source, shoff + i * 40)
                        for i in range(shnum)]
            alloc = next(i for i, section in enumerate(sections) if section[2] & 2)
            mutations = [
                ("section zero PROGBITS", shoff + 4, 1),
                ("noncanonical sentinel", shoff + 20, 1),
                ("names PROGBITS", shoff + shstr * 40 + 4, 1),
                ("missing names", shoff + shstr * 40 + 20, 0),
                ("allocated compressed", shoff + alloc * 40 + 8, sections[alloc][2] | 0x800),
                ("nonallocated compressed", shoff + shstr * 40 + 8, 0x800),
                ("align three", shoff + alloc * 40 + 32, 3),
                ("nonallocated align three", shoff + shstr * 40 + 32, 3),
                ("misaligned address", shoff + alloc * 40 + 32, 0x10000),
                ("name outside table", shoff + alloc * 40, sections[shstr][5]),
            ]
            for label, off, value in mutations:
                changed = bytearray(source)
                struct.pack_into("<I", changed, off, value)
                with self.subTest(slot=slot, mutation=label), self.assertRaises(p.PackageError):
                    p.elf_payload(changed, p.BASES[slot])
            changed = bytearray(source)
            names_off, names_size = sections[shstr][4:6]
            changed[names_off + names_size - 1] = 65
            with self.subTest(slot=slot, mutation="unterminated names"), self.assertRaises(p.PackageError):
                p.elf_payload(changed, p.BASES[slot])
            # Alignment 0/1 means no constraint; powers of two constrain sh_addr,
            # not sh_offset independently. Preserve the real GNU-linked bytes.
            expected = p.elf_payload(source, p.BASES[slot])
            for align in (0, 1, 2, 4):
                changed = bytearray(source)
                struct.pack_into("<I", changed, shoff + alloc * 40 + 32, align)
                self.assertEqual(p.elf_payload(changed, p.BASES[slot]), expected)
            # Nonallocated sh_addr=0 meets even a large alignment; its file
            # offset need not independently meet sh_addralign.
            changed = bytearray(source)
            self.assertNotEqual(sections[shstr][4] % 0x10000, 0)
            struct.pack_into("<I", changed, shoff + shstr * 40 + 32, 0x10000)
            self.assertEqual(p.elf_payload(changed, p.BASES[slot]), expected)
            # NOBITS occupies no file space: its offset is not a file range.
            nobits = next(i for i, section in enumerate(sections) if section[1] == 8)
            struct.pack_into("<I", changed, shoff + nobits * 40 + 16, 0xffffffff)
            self.assertEqual(p.elf_payload(changed, p.BASES[slot]), expected)
            # Names may be empty or reference a suffix; they do not determine
            # whether bytes are loaded. Missing/extended table indexes do fail.
            for name in (0, sections[alloc][0] + 1):
                changed = bytearray(source)
                struct.pack_into("<I", changed, shoff + alloc * 40, name)
                self.assertEqual(p.elf_payload(changed, p.BASES[slot]), expected)
            for off, value in ((44, 0xffff), (48, 0), (50, 0), (50, 0xffff)):
                changed = bytearray(source)
                struct.pack_into("<H", changed, off, value)
                with self.subTest(slot=slot, header=off, value=value), self.assertRaises(p.PackageError):
                    p.elf_payload(changed, p.BASES[slot])

    def test_signed_roundtrip(self):
        if not KEY.exists():
            self.skipTest("external documented demo key unavailable")
        for slot, elf in self.elfs.items():
            output = self.directory / (slot + ".pkg")
            p.package(elf, output, slot, 0xffffffff, KEY, "elf")
            check_signed(self, output.read_bytes(), slot, self.directory)


class BoardLayoutProducerTests(unittest.TestCase):
    """Exercise GNU's implicit PT_LOAD layout, not hand-authored PHDRS."""
    @classmethod
    def setUpClass(cls):
        cls.gcc = fixture_compiler()
        if not cls.gcc:
            raise unittest.SkipTest("arm-none-eabi-gcc unavailable: board producer gate not exercised")
        cls.objcopy = str(Path(cls.gcc).with_name("arm-none-eabi-objcopy"))
        version = command([cls.gcc, "--version"])
        if version.returncode:
            raise RuntimeError("cannot identify ARM fixture compiler")
        print("Board producer compiler: " + version.stdout.decode().splitlines()[0], flush=True)
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        assembly = cls.directory / "board.S"
        assembly.write_text(''' .syntax unified
 .thumb
 .section .isr_vector,"a",%progbits
 .word 0x20010000
 .word Reset_Handler
 .space 228,0
 .section .text,"ax",%progbits
 .thumb_func
 .global Reset_Handler
Reset_Handler: b Reset_Handler
 .section .data,"aw",%progbits
 .byte 0x12,0x34,0x56,0x78,0x9a
 .section .bss,"aw",%nobits
 .space 37
''')
        layout = (ROOT / "linker/lm3s6965.ld").read_text()
        original = "FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K"
        if layout.count(original) != 1 or "PHDRS" in layout:
            raise RuntimeError("board layout changed: re-review the producer fixture transformation")
        cls.elfs = {}
        cls.binaries = {}
        for slot, base in p.BASES.items():
            linker = cls.directory / (slot + ".ld")
            # Only FLASH geometry changes; preserve automatic .data LMA and bss.
            linker.write_text(layout.replace(original,
                f"FLASH (rx)  : ORIGIN = {base:#x}, LENGTH = {p.MAX_PAYLOAD}"))
            for mode, flags in (("supported", ["-Wl,-z,max-page-size=0x100"]),
                                ("default", [])):
                elf = cls.directory / (slot + "-" + mode + ".elf")
                result = command([cls.gcc, "-mcpu=cortex-m3", "-mthumb", "-nostdlib",
                                  "-Wl,--build-id=none", *flags, "-T", str(linker),
                                  str(assembly), "-o", str(elf)])
                if result.returncode:
                    raise RuntimeError("board producer link failed: " + result.stderr.decode())
                binary = elf.with_suffix(".bin")
                result = command([cls.objcopy, "-O", "binary", "--gap-fill", "0xff",
                                  str(elf), str(binary)])
                if result.returncode:
                    raise RuntimeError("board producer objcopy failed: " + result.stderr.decode())
                cls.elfs[slot, mode] = elf
                cls.binaries[slot, mode] = binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_supported_producer_contract(self):
        for slot in p.BASES:
            with self.subTest(slot=slot):
                source = self.elfs[slot, "supported"].read_bytes()
                payload = p.canonical_payload(source, slot, "elf")
                binary = self.binaries[slot, "supported"].read_bytes()
                self.assertEqual(payload, p.canonical_payload(binary, slot, "bin"))
                phoff = struct.unpack_from("<I", source, 28)[0]
                phnum = struct.unpack_from("<H", source, 44)[0]
                loads = [struct.unpack_from("<8I", source, phoff + i * 32)
                         for i in range(phnum)]
                flash = next(h for h in loads if h[0] == 1 and h[2] == p.BASES[slot])
                data = next(h for h in loads if h[0] == 1 and h[2] == 0x20000000)
                self.assertEqual(flash[3], p.BASES[slot])
                self.assertGreater(flash[1], 0)  # ELF headers are not payload.
                self.assertEqual(data[3], p.BASES[slot] + flash[4])
                self.assertEqual(data[4], 8)  # Five initialized bytes + linker alignment.
                self.assertGreater(data[5], data[4])  # bss reservation is retained.
                self.assertEqual(payload[data[3] - p.BASES[slot]:],
                                 bytes.fromhex("123456789a000000"))
                self.assertEqual(len(payload), flash[4] + data[4])  # no bss bytes

    def test_default_layout_portable_raw_fallback(self):
        for slot in p.BASES:
            with self.subTest(slot=slot):
                source = self.elfs[slot, "default"].read_bytes()
                payload = p.canonical_payload(self.binaries[slot, "default"].read_bytes(), slot, "bin")
                expected = p.canonical_payload(self.binaries[slot, "supported"].read_bytes(), slot, "bin")
                self.assertEqual(payload, expected)
                phoff = struct.unpack_from("<I", source, 28)[0]
                phnum = struct.unpack_from("<H", source, 44)[0]
                loads = [struct.unpack_from("<8I", source, phoff + i * 32)
                         for i in range(phnum)]
                outside = any(h[0] == 1 and h[4] and
                              not (p.BASES[slot] <= h[3] and
                                   h[3] + h[4] <= p.BASES[slot] + p.MAX_PAYLOAD)
                              for h in loads)
                # GNU defaults vary: require rejection only when the actual
                # file-backed load map crosses the selected payload boundary.
                print(f"Default board slot {slot}: outside-slot PT_LOAD={outside}", flush=True)
                if outside:
                    with self.assertRaises(p.PackageError):
                        p.canonical_payload(source, slot, "elf")
                else:
                    self.assertEqual(p.canonical_payload(source, slot, "elf"), payload)

    def test_board_signed_roundtrips(self):
        if not KEY.exists():
            self.skipTest("external documented demo key unavailable")
        for slot in p.BASES:
            for label, source, fmt in (
                    ("elf", self.elfs[slot, "supported"], "elf"),
                    ("bin", self.binaries[slot, "supported"], "bin"),
                    ("fallback", self.binaries[slot, "default"], "bin")):
                with self.subTest(slot=slot, producer=label):
                    output = self.directory / (slot + "-" + label + ".pkg")
                    p.package(source, output, slot, 0xffffffff, KEY, fmt)
                    wire = output.read_bytes()
                    self.assertEqual(wire[160:], p.canonical_payload(
                        self.binaries[slot, "supported"].read_bytes(), slot, "bin"))
                    check_signed(self, wire, slot, self.directory)


def assert_verifier_result(test, result, expected):
    stderr = result.stderr.decode(errors="replace")
    test.assertEqual(result.returncode, expected,
                     f"verifier expected exit {expected}, got {result.returncode}; stderr: {stderr}")
    test.assertNotRegex(stderr, r"(?i)(sanitizer|runtime error:)",
                        f"verifier sanitizer diagnostic: {stderr}")


class VerifierOracleTests(unittest.TestCase):
    def test_crash_and_sanitizer_are_not_rejections(self):
        for code, stderr in ((-11, b"simulated SIGSEGV"), (139, b"simulated shell signal"),
                             (1, b"ERROR: AddressSanitizer: simulated"),
                             (1, b"runtime error: simulated undefined behavior")):
            with self.subTest(code=code, stderr=stderr), self.assertRaises(AssertionError):
                assert_verifier_result(self, subprocess.CompletedProcess([], code, b"", stderr), 1)
        assert_verifier_result(self, subprocess.CompletedProcess([], 1, b"", b"invalid package"), 1)


def check_signed(test, wire, slot, directory):
    length = struct.unpack_from("<I", wire, 36)[0]
    test.assertEqual(len(wire), 160 + length)
    test.assertEqual(wire[:16], b"LM3S6965-FW-V1\0\0")
    test.assertEqual(struct.unpack_from("<8I", wire, 16),
                     (1, 160, 1, int(slot == "B"), p.BASES[slot], length, 0xffffffff, 0))
    test.assertEqual(wire[80:96], bytes(16))
    test.assertEqual(wire[48:80], hashlib.sha256(wire[160:]).digest())
    # Independent OpenSSL message-input verify hashes prefix ONCE; does not use
    # package.py's digest helper or DER decoder to construct its verify input.
    integers = []
    for start in (96, 128):
        value = int.from_bytes(wire[start:start + 32], "big")
        encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
        if encoded[0] & 0x80:
            encoded = b"\0" + encoded
        integers.append(b"\x02" + bytes([len(encoded)]) + encoded)
    body = b"".join(integers)
    signature = directory / "signature.der"
    signature.write_bytes(b"\x30" + bytes([len(body)]) + body)
    public = directory / "public.der"
    public.write_bytes(p.SPKI_PREFIX + p.trusted_public())
    prefix = directory / "prefix"
    prefix.write_bytes(wire[:96])
    result = command(["openssl", "dgst", "-sha256", "-verify", str(public), "-keyform", "DER",
                      "-signature", str(signature), str(prefix)])
    test.assertEqual(result.returncode, 0, result.stderr.decode())
    if VERIFIER:
        package = directory / "verify.pkg"
        package.write_bytes(wire)
        assert_verifier_result(test, command([VERIFIER, slot, str(package)]), 0)
        assert_verifier_result(test, command([VERIFIER, "B" if slot == "A" else "A", str(package)]), 1)
        for changed in (wire[:159], wire[:-1], wire + b"\xff", wire + bytes(864),
                        wire[:40] + bytes([wire[40] ^ 1]) + wire[41:],
                        wire[:96] + bytes([wire[96] ^ 1]) + wire[97:],
                        wire[:-1] + bytes([wire[-1] ^ 1])):
            package.write_bytes(changed)
            assert_verifier_result(test, command([VERIFIER, slot, str(package)]), 1)


class SigningSafetyTests(unittest.TestCase):
    def test_external_path_policy(self):
        with self.assertRaises(p.PackageError):
            p.validate_key(Path("relative.pem"))
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            key = Path(directory) / "key.pem"
            key.write_text("not private material")
            key.chmod(0o600)
            with self.assertRaises(p.PackageError):
                p.validate_key(key)
        with tempfile.TemporaryDirectory() as directory:
            key = Path(directory) / "key.pem"
            key.write_text("not a key")
            key.chmod(0o600)
            with self.assertRaises(p.PackageError):
                p.validate_key(key)
            link = Path(directory) / "alias.pem"
            link.symlink_to(key)
            with self.assertRaises(p.PackageError):
                p.validate_key(link)
            key.chmod(0o644)
            with self.assertRaises(p.PackageError):
                p.validate_key(key)

    def test_wrong_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            key = Path(directory) / "key.pem"
            for curve in ("prime256v1", "secp384r1"):
                # OpenSSL writes directly to external 0700 temporary directory.
                result = command(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                                  "ec_paramgen_curve:" + curve, "-out", str(key)])
                self.assertEqual(result.returncode, 0)
                key.chmod(0o600)
                with self.assertRaises(p.PackageError):
                    p.validate_key(key)

    def test_output_and_signing_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source, output = folder / "image.bin", folder / "image.pkg"
            source.write_bytes(raw())
            # Existing, owned NONSECRET file: samefile checks must not depend on
            # an external private key or HOME for these mocked failure paths.
            dummy = folder / "nonsecret-dummy.pem"
            dummy.write_text("not private material\n")
            with patch.object(p, "validate_key", return_value=dummy.resolve()), \
                 patch.object(p, "openssl_run", side_effect=p.PackageError("OpenSSL operation failed")):
                with self.assertRaises(p.PackageError):
                    p.package(source, output, "A", 1, dummy, "bin")
            self.assertFalse(output.exists())
            self.assertFalse(list(folder.glob(".boot-package-*")))
            with patch.object(p, "validate_key", return_value=dummy.resolve()), \
                 patch.object(p, "openssl_run", return_value=b"malformed DER"):
                with self.assertRaises(p.PackageError):
                    p.package(source, output, "A", 1, dummy, "bin")
            self.assertFalse(output.exists())
            self.assertFalse(list(folder.glob(".boot-package-*")))
            with self.assertRaises(p.PackageError):
                p.package(source, source, "A", 1, dummy, "bin")
            self.assertEqual(source.read_bytes(), raw())
            alias = folder / "input-alias"
            alias.symlink_to(source)
            with self.assertRaises(p.PackageError):
                p.package(source, alias, "A", 1, dummy, "bin")
            with self.assertRaises(p.PackageError):
                p.package(source, dummy, "A", 1, dummy, "bin")
            with patch.object(p, "validate_key", return_value=source.resolve()):
                with self.assertRaises(p.PackageError):
                    p.package(source, output, "A", 1, source.resolve(), "bin")

    def test_signed_raw_boundaries(self):
        if not KEY.exists():
            self.skipTest("external documented demo key unavailable")
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for slot in p.BASES:
                for size in (236, 237, 111616):
                    source = folder / "image.bin"
                    source.write_bytes(raw(slot, size))
                    output = folder / (slot + str(size) + ".pkg")
                    result = command([sys.executable, str(ROOT / "tools/boot/package.py"), "--input", str(source),
                                      "--output", str(output), "--slot", slot, "--release", "4294967295",
                                      "--private-key", str(KEY), "--input-format", "bin"])
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    signed = output.read_bytes()
                    check_signed(self, signed, slot, folder)
                    with self.assertRaises(p.PackageError):
                        p.package(source, output, slot, 1, KEY, "bin")
                    alias = folder / "key-input-alias"
                    if not alias.exists():
                        alias.symlink_to(KEY)
                    with self.assertRaises(p.PackageError):
                        p.package(alias, folder / "key-as-input.pkg", slot, 1, KEY, "bin")
                    self.assertEqual(output.read_bytes(), signed)
                    self.assertNotEqual(command([sys.executable, str(ROOT / "tools/boot/package.py"),
                        "--input", str(source), "--output", str(output), "--slot", slot,
                        "--release", "-1", "--private-key", str(KEY), "--input-format", "bin"]).returncode, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier")
    args, remaining = parser.parse_known_args()
    VERIFIER = args.verifier
    unittest.main(argv=[sys.argv[0], *remaining])
