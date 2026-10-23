"""Actual Release cross-link plus bounded resource-parser negative tests."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from resource_probe.check import check_map, inspect
import test_layout

ROOT = test_layout.ROOT


class ResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_layout.LayoutTests.setUpClass()
        cls.temp = tempfile.TemporaryDirectory(prefix="boot-resource-")
        build = Path(cls.temp.name)
        subprocess.run(["cmake", "-S", str(ROOT / "tests/boot/resource_probe"), "-B", str(build),
                        "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "cmake/toolchain-lm3s6965.cmake"),
                        "-DCMAKE_C_COMPILER=" + test_layout.LayoutTests.gcc, "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", "8"], check=True)
        cls.data = (build / "resource_probe").read_bytes()
        cls.map = (build / "resource_probe.map").read_text()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        test_layout.LayoutTests.tearDownClass()

    def test_actual_release_probe(self):
        result = inspect(self.data)
        check_map(self.map)
        self.assertGreater(result["flash_margin"], 0)
        print("Actual Arm Release resource probe:", result, flush=True)

    def test_truncation_and_bad_tables(self):
        for length in (0, 7, 51, 100, len(self.data) - 1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                inspect(self.data[:length])
        for offset, fmt, value in ((28, "I", 0xffffffff), (32, "I", 0xffffffff),
                                   (44, "H", 65535), (48, "H", 65535), (50, "H", 65535)):
            data = bytearray(self.data)
            struct.pack_into("<" + fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect(data)

    def test_load_bounds_and_file_backed_diagnostics(self):
        phoff = struct.unpack_from("<I", self.data, 28)[0]
        phnum = struct.unpack_from("<H", self.data, 44)[0]
        for i in range(phnum):
            off = phoff + i * 32
            typ, _, vma, _, filesz, _, _, _ = struct.unpack_from("<8I", self.data, off)
            if typ != 1:
                continue
            data = bytearray(self.data)
            if filesz:
                struct.pack_into("<I", data, off + 12, 0x8000)
            else:
                struct.pack_into("<I", data, off + 16, 1)
            with self.subTest(vma=hex(vma)), self.assertRaises(ValueError):
                inspect(data)

    def test_missing_closure_and_map_owner(self):
        for name in (b"boot_verify_stored", b"lm3s6965_boot_demo_public_key", b"calloc"):
            data = self.data.replace(b"\0" + name + b"\0", b"\0" + b"x" * len(name) + b"\0")
            self.assertNotEqual(data, self.data)
            with self.subTest(symbol=name), self.assertRaises(ValueError):
                inspect(data)
        with self.assertRaises(ValueError):
            check_map(self.map.replace("syscalls.c.obj", "fake.c.obj"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
