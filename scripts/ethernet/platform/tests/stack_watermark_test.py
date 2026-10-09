"""Native, device-free fixtures for the production binary stack scanner."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "stack_watermark.py"
SPEC = importlib.util.spec_from_file_location("stack_watermark", MODULE_PATH)
scanner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = scanner
SPEC.loader.exec_module(scanner)
SIZE = scanner.ESTACK - scanner.STACK_LIMIT


class StackWatermarkTest(unittest.TestCase):
    def pristine(self):
        return bytearray(scanner.SENTINEL_BYTES * (SIZE // 4))

    def check(self, data, offset):
        result = scanner.scan(bytes(data))
        self.assertEqual(result.first_mismatch, scanner.STACK_LIMIT + offset)
        self.assertEqual(result.written_watermark_bytes, SIZE - offset)
        self.assertEqual(result.untouched_prefix_bytes, offset)

    def test_explicit_little_endian(self):
        self.assertEqual(scanner.SENTINEL_BYTES, b"\x71\x9e\xc3\xa5")
        big_endian = scanner.SENTINEL.to_bytes(4, "big") * (SIZE // 4)
        self.check(big_endian, 0)

    def test_pristine(self):
        result = scanner.scan(bytes(self.pristine()))
        self.assertIsNone(result.first_mismatch)
        self.assertEqual(result.written_watermark_bytes, 0)
        self.assertEqual(result.untouched_prefix_bytes, SIZE)

    def test_all_changed_and_boundary(self):
        self.check(bytes(SIZE), 0)
        data = self.pristine()
        data[0] ^= 1
        self.check(data, 0)

    def test_changed_suffix(self):
        data = self.pristine()
        data[-128:] = bytes(128)
        self.check(data, SIZE - 128)

    def test_unaligned_first_byte(self):
        for offset in (1, 2, 3, SIZE - 7, SIZE - 1):
            with self.subTest(offset=offset):
                data = self.pristine()
                data[offset:] = bytes(SIZE - offset)
                self.check(data, offset)

    def test_isolated_mismatch(self):
        data = self.pristine()
        data[137] ^= 1
        self.check(data, 137)

    def test_sentinel_holes(self):
        data = self.pristine()
        data[1023] ^= 1
        data[4096:4100] = bytes(4)
        data[-4:] = bytes(4)
        self.check(data, 1023)

    def test_bad_length(self):
        for size in (0, 4, SIZE - 1, SIZE + 1, SIZE * 2):
            with self.subTest(size=size), self.assertRaises(ValueError):
                scanner.scan(bytes(size))

    def test_bad_bounds(self):
        for start, end in ((0, SIZE), (scanner.STACK_LIMIT + 1, scanner.ESTACK),
                           (scanner.STACK_LIMIT, scanner.ESTACK - 1),
                           (scanner.ESTACK, scanner.STACK_LIMIT), (-1, -1)):
            with self.subTest(start=start, end=end), self.assertRaises(ValueError):
                scanner.scan(bytes(self.pristine()), start, end)

    def test_cli_valid_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            dump = Path(directory) / "stack.bin"
            dump.write_bytes(self.pristine())
            for args, expected in (([str(dump)], 0),
                                   ([str(dump), "--start", "0"], 2),
                                   ([str(dump), "--end", "0x20010004"], 2),
                                   ([str(dump) + ".missing"], 2)):
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    self.assertEqual(scanner.main(args), expected)
                if expected:
                    self.assertEqual(out.getvalue(), "")
                    self.assertIn("ERROR:", err.getvalue())
                else:
                    self.assertIn("written watermark: 0 bytes", out.getvalue())
                    self.assertIn("Not exact peak", out.getvalue())
            dump.write_bytes(bytes(SIZE - 1))
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(scanner.main([str(dump)]), 2)
            self.assertEqual(out.getvalue(), "")
            self.assertIn("exactly 8192", err.getvalue())


if __name__ == "__main__":
    unittest.main()
