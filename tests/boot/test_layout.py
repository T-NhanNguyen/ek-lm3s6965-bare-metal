"""Real GNU Arm link/load contracts. Required tools: never skip fixtures."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("package", ROOT / "tools/boot/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
SCRIPTS = {
    "bootloader": ("linker/bootloader.ld", 0, 0x8000),
    "A": ("linker/app_a.ld", 0x8800, 0x23c00),
    "B": ("linker/app_b.ld", 0x24000, 0x3f400),
    "standalone": ("linker/lm3s6965.ld", 0, 0x40000),
    "ftp": ("examples/ethernet-ftp/lm3s6965_ftp.ld", 0, 0x40000),
}


def elf_tables(data):
    """Independent ELF32 section/symbol/PT_LOAD reader for GNU fixtures."""
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phsize, phnum, shsize, shnum, shstr = struct.unpack_from("<5H", data, 42)
    headers = [struct.unpack_from("<10I", data, shoff + i * shsize) for i in range(shnum)]
    strings = data[headers[shstr][4]:headers[shstr][4] + headers[shstr][5]]
    def name(table, index):
        return table[index:table.index(b"\0", index)].decode()
    sections = {name(strings, h[0]): h for h in headers[1:]}
    symbols = {}
    for h in headers:
        if h[1] != 2:
            continue
        table = headers[h[6]]
        names = data[table[4]:table[4] + table[5]]
        for off in range(h[4], h[4] + h[5], h[9]):
            index, value, size, info, other, section = struct.unpack_from("<IIIBBH", data, off)
            symbols[name(names, index)] = value
    loads = [struct.unpack_from("<8I", data, phoff + i * phsize) for i in range(phnum)]
    return sections, symbols, [h for h in loads if h[0] == 1]


class LayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Match existing packager's official-toolchain preference; fail if absent.
        candidates = sorted((Path.home() / ".toolchains").glob(
            "arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc"))
        candidates += sorted(Path("/Applications/ArmGNUToolchain").glob(
            "*/arm-none-eabi/bin/arm-none-eabi-gcc"))
        cls.gcc = os.environ.get("BOOT_ARM_GCC") or (str(candidates[-1]) if candidates else
                                                   shutil.which("arm-none-eabi-gcc"))
        if not cls.gcc:
            raise RuntimeError("GNU Arm compiler required for layout tests")
        cls.objcopy = subprocess.check_output([cls.gcc, "-print-prog-name=objcopy"], text=True).strip()
        print("Layout GNU compiler: " + cls.gcc, flush=True)
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.serial = 0

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def link(self, key, definitions=(), script=None, succeeds=True, expected_error=None,
             gc_sections=False, source=None, extra_options=()):
        type(self).serial += 1
        elf = self.directory / (str(self.serial) + ".elf")
        command = [self.gcc, "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft",
                   "-nostdlib", "-Wl,-z,max-page-size=0x100", "-Wl,-Map=" + str(elf.with_suffix(".map")),
                   "-T" + str(script or ROOT / SCRIPTS[key][0]),
                   *["-D" + d for d in definitions],
                   str(source or ROOT / "tests/boot/layout_fixture.S"),
                   *extra_options, "-o", str(elf)]
        if gc_sections:
            command.insert(1, "-Wl,--gc-sections")
        result = subprocess.run(command, capture_output=True, text=True)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stderr)
            return elf
        self.assertNotEqual(result.returncode, 0, "negative fixture unexpectedly linked")
        self.assertIsNotNone(expected_error, "negative fixtures need a specific error oracle")
        self.assertRegex(result.stderr, expected_error)
        return result.stderr

    def check_map(self, elf, key, require_gap=False):
        data = elf.read_bytes()
        sections, symbols, loads = elf_tables(data)
        base, limit = SCRIPTS[key][1:]
        expected = {
            "_estack": 0x20010000, "_stack_limit": 0x2000e000,
            "_diagnostic_start": 0x2000dfc0, "_diagnostic_end": 0x2000e000,
            "_flash_payload_start": base, "_flash_payload_limit": limit,
            "_vectors_start": base, "_vectors_end": base + 236,
            "_sdata": 0x20000000, "_edata": 0x20000010,
            "_sbss": 0x20000010, "_ebss": 0x20000030,
            "_end": 0x20000030, "_heap_start": 0x20000030,
            "_heap_static_end": 0x20000050,
            "_heap_end": 0x20002030 if key == "ftp" else 0x2000dfc0,
        }
        for symbol, value in expected.items():
            self.assertEqual(symbols[symbol], value, (key, symbol))
        if key == "bootloader":
            self.assertEqual(symbols["_metadata_start"], 0x8000)
            self.assertEqual(symbols["_metadata_end"], 0x8800)
        if key in ("A", "B"):
            self.assertEqual(symbols["_manifest_start"], limit)
            self.assertEqual(symbols["_manifest_end"], limit + 1024)
        vector = sections[".isr_vector"]
        self.assertEqual((vector[1], vector[3], vector[5], vector[8]), (1, base, 236, 256))
        msp, reset = struct.unpack_from("<II", data, vector[4])
        self.assertEqual(msp, 0x20010000)
        self.assertEqual(reset & 1, 1)
        self.assertEqual(reset & ~1, sections[".text"][3])
        self.assertEqual((sections[".text"][3], sections[".text"][5]), (base + 236, 24))
        self.assertEqual((sections[".data"][1], sections[".data"][3], sections[".data"][5]),
                         (1, 0x20000000, 16))
        self.assertEqual(symbols["_sidata"], base + (0x800 if require_gap else 284))
        self.assertEqual(symbols["_flash_payload_end"], symbols["_sidata"] + 16)
        for section, addr, size in ((".boot_diagnostic", 0x2000dfc0, 64),
                                    (".stack", 0x2000e000, 8192)):
            h = sections[section]
            self.assertEqual((h[1], h[3], h[5]), (8, addr, size))
        for section in (".bss", ".c_heap"):
            self.assertEqual(sections[section][1], 8)
            self.assertLessEqual(sections[section][3] + sections[section][5], 0x2000dfc0)
        flash_end = base
        for typ, off, vma, lma, filesz, memsz, flags, align in loads:
            self.assertEqual(align, 256)
            self.assertEqual(vma % align, off % align)
            if filesz:
                self.assertGreaterEqual(lma, base)
                self.assertLessEqual(lma + filesz, limit)
                flash_end = max(flash_end, lma + filesz)
                if vma >= 0x20000000:
                    self.assertEqual((vma, filesz), (symbols["_sdata"], 16))
                    self.assertEqual(lma, symbols["_sidata"])
                else:
                    self.assertEqual(vma, lma)
            else:
                self.assertGreaterEqual(vma, 0x20000000)
                self.assertLessEqual(vma + memsz, 0x20010000)
            # File-backed segment memory extension cannot cover diagnostics.
            if filesz and vma >= 0x20000000:
                self.assertLessEqual(vma + memsz, 0x2000dfc0)
        self.assertEqual(symbols["_flash_payload_end"], flash_end)
        # Every allocated section has exactly one PT_LOAD mapping; NOBITS is
        # never file backed, including diagnostics and optional static heap.
        for h in sections.values():
            if not h[2] & 2 or not h[5]:
                continue
            matching = [p for p in loads if p[2] <= h[3] and h[3] + h[5] <= p[2] + p[5]]
            self.assertEqual(len(matching), 1, (key, h))
            p = matching[0]
            delta = h[3] - p[2]
            if h[1] == 8:
                self.assertGreaterEqual(delta, p[4])
            else:
                self.assertLessEqual(delta + h[5], p[4])
                self.assertEqual(h[4], p[1] + delta)
        binary = elf.with_suffix(".bin")
        subprocess.run([self.objcopy, "--gap-fill=0xff", "-O", "binary", str(elf), str(binary)], check=True)
        raw = binary.read_bytes()
        self.assertEqual(len(raw), flash_end - base)
        covered = bytearray(len(raw))
        for p in loads:
            if p[4]:
                start = p[3] - base
                self.assertEqual(raw[start:start + p[4]], data[p[1]:p[1] + p[4]])
                covered[start:start + p[4]] = b"\1" * p[4]
        if require_gap:
            self.assertIn(0, covered)  # The gap fixture must really exercise FF filling.
        for i, present in enumerate(covered):
            if not present:
                self.assertEqual(raw[i], 255)
        for section in (".preinit_array", ".init_array", ".fini_array", ".ctors", ".dtors"):
            self.assertEqual(sections[section][5], 4)
        if key in ("A", "B"):
            self.assertEqual(package.canonical_payload(data, key, "elf"), raw)
            self.assertEqual(package.canonical_payload(raw, key, "bin"), raw)
        return sections, symbols, loads

    def test_all_five_load_maps_and_slot_packager_acceptance(self):
        for key in SCRIPTS:
            with self.subTest(script=key):
                self.check_map(self.link(key), key)

    def test_objcopy_ff_gaps_and_packager(self):
        # Exercise explicit separated data LMA using otherwise unchanged scripts.
        # GNU AT> placement need not create a gap merely for VMA alignment.
        for key, (path, base, _) in SCRIPTS.items():
            with self.subTest(script=key):
                script = self.directory / (key + "-gap.ld")
                script.write_text((ROOT / path).read_text().replace(
                    ".data :", f".data : AT({base + 0x800:#x})").replace(
                    "> SRAM AT> FLASH", "> SRAM"))
                self.check_map(self.link(key, script=script), key, require_gap=True)

    def test_unsupported_allocated_sections_rejected(self):
        # Retained-only contract: every live/no-GC unknown must fail. Dead
        # PROGBITS/NOBITS may disappear only if absent from the final ELF.
        # GNU ld retains allocated NOTE even when unreferenced: that is an
        # intended assertion rejection, NOT a requirement to force-discard it.
        kinds = ((".unknown_aw", "aw", "progbits"),
                 (".unknown_ro", "a", "progbits"),
                 (".unknown_exec", "ax", "progbits"),
                 (".unknown_bss", "aw", "nobits"),
                 (".note.unknown", "a", "note"),
                 (".comment", "a", "progbits"))
        passed = 0
        for key in SCRIPTS:
            for section, flags, kind in kinds:
                for mode in ("reference", "undefined", "nogc", "dead"):
                    with self.subTest(script=key, section=section, mode=mode):
                        source = self.directory / "orphan-proof.S"
                        source.write_text(
                            '.syntax unified\n.thumb\n'
                            '.section .isr_vector,"a",%progbits\n.balign 256\n'
                            '.word 0x20010000\n.word Reset_Handler\n.space 228\n'
                            '.section .text.main,"ax",%progbits\n.global main\n'
                            '.global Reset_Handler\n.thumb_func\nReset_Handler:\nmain:\n'
                            ' b Reset_Handler\n' +
                            ('.word unknown\n' if mode == "reference" else '') +
                            f'.section {section},"{flags}",%{kind}\n'
                            '.global unknown\nunknown:\n.space 16\n')
                        retained = mode != "dead" or kind == "note"
                        elf = self.link(
                            key, source=source, succeeds=not retained,
                            expected_error="unsupported allocated input section",
                            gc_sections=mode != "nogc",
                            extra_options=("-Wl,-u,unknown",) if mode == "undefined" else ())
                        if not retained:
                            sections, symbols, _ = elf_tables(elf.read_bytes())
                            self.assertNotIn(section, sections)
                            self.assertNotIn("unknown", symbols)
                            self.assertNotIn(".unsupported_allocated", sections)
                        passed += 1
        self.assertEqual(passed, 120)
        print("120 retained-only proofs: 90 live/-u/no-GC rejections, "
              "25 dead ELF absences, 5 dead allocated NOTE assertion rejections")

    def test_nonallocated_metadata_and_debug_preserved(self):
        for key in SCRIPTS:
            with self.subTest(script=key):
                elf = self.link(key, ["EXTRA_METADATA"])
                sections, _, _ = self.check_map(elf, key)
                for name in (".extra_metadata", ".debug_extra", ".ARM.attributes"):
                    self.assertIn(name, sections)
                    self.assertEqual(sections[name][2] & 2, 0)
                self.assertNotIn(".note.extra", sections)

    def test_flash_text_overflow(self):
        for key, (_, base, limit) in SCRIPTS.items():
            with self.subTest(script=key):
                self.link(key, [f"TEXT_BYTES={limit - base}"], succeeds=False,
                          expected_error="flash.*exceeds partition")

    def test_flash_data_lma_overflow(self):
        # .data itself fits SRAM, only the aggregate flash LMA crosses reserve.
        for key, (_, base, limit) in SCRIPTS.items():
            with self.subTest(script=key):
                self.link(key, [f"TEXT_BYTES={limit - base - 1024}", "DATA_BYTES=2048"],
                          succeeds=False, expected_error="flash.*exceeds partition")

    def test_bss_overflow_into_diagnostics(self):
        for key in SCRIPTS:
            with self.subTest(script=key):
                self.link(key, ["BSS_BYTES=0xdfc0"], succeeds=False,
                          expected_error="(SRAM.*diagnostic|data/bss overlaps diagnostic)")

    def test_static_heap_overflow_into_diagnostics(self):
        for key in SCRIPTS:
            with self.subTest(script=key):
                self.link(key, ["HEAP_BYTES=0xdfc0"], succeeds=False,
                          expected_error="(SRAM.*diagnostic|heap overlaps diagnostic)")

    def test_vectors_too_short_and_misaligned(self):
        for key, (path, _, _) in SCRIPTS.items():
            with self.subTest(script=key):
                self.link(key, ["VECTOR_BYTES=232"], succeeds=False,
                          expected_error="vectors must contain at least 59 words")
                script = self.directory / (key + "-misaligned.ld")
                script.write_text((ROOT / path).read_text().replace(
                    ".isr_vector ORIGIN(FLASH)", ".isr_vector (ORIGIN(FLASH) + 4)"))
                self.link(key, script=script, succeeds=False,
                          expected_error="vectors must start at the 256-byte-aligned payload origin")

    def test_wrong_msp_rejected_by_artifact_validation_not_ld(self):
        # GNU ld cannot inspect vector contents. Enforce the symbol in scripts,
        # then verify actual bytes with the existing packager before signing.
        for key in ("A", "B"):
            elf = self.link(key, ["INITIAL_MSP=0x2000fff8"])
            with self.assertRaisesRegex(package.PackageError, "initial MSP"):
                package.canonical_payload(elf.read_bytes(), key, "elf")

    def test_constructor_flash_overflow(self):
        for key, (_, base, limit) in SCRIPTS.items():
            with self.subTest(script=key):
                self.link(key, [f"CTOR_BYTES={limit - base}"], succeeds=False,
                          expected_error="flash.*exceeds partition")

    def test_ftp_static_heap_fixed_capacity(self):
        elf = self.link("ftp", ["HEAP_BYTES=8192"])
        _, symbols, _ = elf_tables(elf.read_bytes())
        self.assertEqual(symbols["_heap_end"] - symbols["_heap_start"], 8192)
        self.assertEqual(symbols["_heap_static_end"], symbols["_heap_end"])
        self.link("ftp", ["HEAP_BYTES=8200"], succeeds=False,
                  expected_error="FTP static heap exceeds fixed 8 KiB reserve")

    def test_no_manifest_metadata_load(self):
        for key in ("bootloader", "A", "B"):
            for definition in ("FORBIDDEN=1", "FORBIDDEN_METADATA=1"):
                with self.subTest(script=key, definition=definition):
                    self.link(key, [definition], succeeds=False,
                              expected_error="manifest/metadata must not be firmware load bytes")


if __name__ == "__main__":
    unittest.main(verbosity=2)
