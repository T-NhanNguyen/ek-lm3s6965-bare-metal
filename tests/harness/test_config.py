"""Offline configuration regression only; never launch product/tool processes."""
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("host_integrity_runner", Path(__file__).with_name("runner.py"))
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

EXPECTED_CTEST = frozenset("""
boot_dependencies boot_allocator boot_receive boot_service boot_pc_workflow
boot_pc_ftp boot_handoff boot_handoff_target boot_watchdog boot_flash
boot_diag_publication boot_diag_publication_target boot_diag boot_reset_handler
boot_reset boot_lifecycle boot_store boot_package boot_layout boot_resources
boot_resource_execution boot_receive_fault boot_format boot_crypto boot_verify
boot_verify_fault boot_snapshot boot_launch boot_launch_fault boot_select
boot_select_fault boot_runtime boot_runtime_fault boot_runtime_watchdog
boot_runtime_watchdog_fault
""".split())


class HarnessConfigTests(unittest.TestCase):
    def test_exact_ctest_roster(self):
        names = runner.ALL_CASES["ctest"]
        self.assertEqual(len(names), 35)
        self.assertEqual(set(names), EXPECTED_CTEST)
        # Static registration check; actual configured roster is also checked by
        # prepare(). No CMake configuration or test discovery subprocess here.
        registered = []
        for directory in ("boot", "boot_launch"):
            text = (runner.ROOT / "tests" / directory / "CMakeLists.txt").read_text()
            for name, variant in re.findall(r"add_test\(NAME\s+(\w+)(\$\{variant\})?", text):
                # boot_launch registers its three compositions in the explicit
                # foreach(variant IN ITEMS "" "_fault") loops.
                registered.extend([name, name + "_fault"] if variant else [name])
        self.assertEqual(len(registered), 35)
        self.assertEqual(set(registered), EXPECTED_CTEST)

    def test_selection_accounting_and_offline_ftp_intent(self):
        roster = [target + ":" + name for target, names in runner.ALL_CASES.items() for name in names]
        self.assertEqual(len(roster), 54)
        self.assertEqual(len(set(roster)), 54)
        self.assertEqual(runner.DATA["merged_cases"], {"ethernet:lwip": "ethernet:lwip-boot"})
        selected = [name for name in roster if name not in runner.DATA["merged_cases"]]
        self.assertEqual(len(selected), 53)
        self.assertIn("ctest:boot_pc_ftp", selected)
        self.assertIn("ctest:boot_resource_execution", selected)
        for rows in runner.DATA["targets"].values():
            for row in rows:
                self.assertEqual(len(row), 3)
                self.assertTrue(all(row))

    def test_content_exclusion_boundary(self):
        for name in ("resume.tex", "work/resume.tex", ".dev-vault/secret", ".pi/local",
                     ".git/config", "out/build/a", "build-host/a", "x/__pycache__/a", "a.pyc"):
            with self.subTest(name=name):
                self.assertTrue(runner.identity_excluded(name))
        for name in ("tests/boot/test_resource_execution.py", "src/boot_receive.c",
                     "tests/harness/manifest.json", "docs/boot-receive.md"):
            with self.subTest(name=name):
                self.assertFalse(runner.identity_excluded(name))

    def test_identity_excludes_before_content_read_and_does_not_follow_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "public.c").write_text("public\n")
            (root / "resume.tex").write_text("private sentinel\n")
            (root / "link").symlink_to("resume.tex")
            outputs = [b"public.c\0resume.tex\0.dev-vault/private\0build/a\0link\0",
                       b"fixture-head\n", b"?? resume.tex\n", b""]
            read_bytes = Path.read_bytes
            def public_only(path):
                self.assertEqual(path, root / "public.c", "private/link content must never be read")
                return read_bytes(path)
            with mock.patch.object(runner, "ROOT", root), \
                 mock.patch.object(runner.subprocess, "check_output", side_effect=outputs), \
                 mock.patch.object(Path, "read_bytes", public_only):
                provenance = runner.identity()
            self.assertEqual(set(provenance["files"]), {"public.c", "link"})
            self.assertTrue(provenance["files"]["link"].startswith("symlink:"))
            self.assertIn("resume.tex", provenance["status"])


if __name__ == "__main__":
    unittest.main()
