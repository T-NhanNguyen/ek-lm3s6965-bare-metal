"""Offline CLI regression: invalid arguments must never reach build tools."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[2] / "test.sh"
SUITES = ["core", "tcp", "lwip", "file", "platform", "consumer", "driver", "raw", "link"]


class RunnerCliTests(unittest.TestCase):
    def invoke(self, args):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "work-started"
            # Catch compilers, temporary setup, SDK tools, and Python/CMake too.
            fake = root / "fake-tool"
            fake.write_text('#!/bin/sh\nprintf work >> "$WORK_MARKER"\nexit 99\n')
            fake.chmod(0o755)
            for name in ("cc", "cmake", "python3", "xcrun", "mktemp", "uname"):
                (root / name).symlink_to(fake)
            env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}",
                       CC=str(fake), WORK_MARKER=str(marker))
            result = subprocess.run(["/bin/bash", str(RUNNER), *args],
                                    env=env, capture_output=True, text=True)
            self.assertFalse(marker.exists(), result.stdout + result.stderr)
            return result

    def test_help(self):
        result = self.invoke(["--help"])
        self.assertEqual(result.returncode, 0)
        self.assertIn("offline", result.stdout)

    def test_list(self):
        result = self.invoke(["--list"])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.splitlines(), SUITES)

    def test_all_arguments_validated_before_work(self):
        for args in (["unknown"], ["core", "unknown"], ["all", "--live"],
                     ["ftp", "en7"], ["--help", "core"], ["--list", "unknown"],
                     ["raw", "--self-test"], ["link", "--seconds", "1"], [""]):
            with self.subTest(args=args):
                result = self.invoke(args)
                self.assertEqual(result.returncode, 2)
                self.assertIn("Unknown suite or option", result.stderr)


if __name__ == "__main__":
    unittest.main()
