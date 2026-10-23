#!/usr/bin/env python3
"""Isolated setup contract tests; never touch the checkout's real .env or keys."""
import hashlib
import importlib.util
import os
from pathlib import Path
import runpy
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
OPENSSL = shutil.which("openssl")


class SetupKeysTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.repo = self.base / "checkout"
        for name in ("scripts/setup-boot-demo-keys.py", "scripts/build_ab_boot_demo",
                     "tools/boot/package.py", "config/boot_demo_public_key.h"):
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.script = self.repo / "scripts/setup-boot-demo-keys.py"
        self.output = self.base / "checkout-keys"

    def run_setup(self, *args, env=None):
        return subprocess.run([sys.executable, str(self.script), *map(str, args)],
                              cwd=self.base, env=env, text=True, capture_output=True,
                              timeout=30)

    def require_openssl(self):
        if OPENSSL is None:
            self.skipTest("installed OpenSSL required for ephemeral key tests")

    def test_default_sibling_pair_and_env(self):
        self.require_openssl()
        result = self.run_setup("--init-env")
        self.assertEqual(result.returncode, 0, result.stderr)
        key = self.output / "signing-key.pem"
        public = self.output / "authorized-public.sec1"
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((self.repo / ".env").stat().st_mode), 0o600)
        self.assertEqual(len(public.read_bytes()), 65)
        self.assertEqual(public.read_bytes()[0], 4)
        spec = importlib.util.spec_from_file_location("isolated_package", self.repo / "tools/boot/package.py")
        tools = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tools)
        self.assertEqual(tools.validate_key(key, public), key)
        self.assertNotEqual(public.read_bytes(), tools.trusted_public())
        self.assertIn("Public key SHA256 (raw SEC1): " +
                      hashlib.sha256(public.read_bytes()).hexdigest(), result.stdout)
        config = runpy.run_path(str(self.repo / "scripts/build_ab_boot_demo"))["configuration"](self.repo)
        self.assertEqual(config["LM3S6965_BOOT_RELEASE_A"], "1")
        self.assertEqual(config["LM3S6965_BOOT_RELEASE_B"], "2")
        self.assertEqual(config["LM3S6965_BOOT_PRIVATE_KEY_FILE"], str(key))
        self.assertEqual(config["LM3S6965_BOOT_PUBLIC_KEY_FILE"], str(public))
        self.assertNotIn("PRIVATE KEY", result.stdout + result.stderr)
        self.assertFalse(list(self.repo.rglob("*generated*")))

    def test_override_existing_safe_directory_with_spaces(self):
        self.require_openssl()
        output = self.base / "external identity"
        output.mkdir(mode=0o700)
        result = self.run_setup("--out-dir", output, "--init-env")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists())
        config = runpy.run_path(str(self.repo / "scripts/build_ab_boot_demo"))["configuration"](self.repo)
        self.assertEqual(config["LM3S6965_BOOT_PRIVATE_KEY_FILE"], str(output / "signing-key.pem"))

    def test_existing_env_refused_before_generation(self):
        env = self.repo / ".env"
        original = b"# lost key, deliberately preserved\nPRIVATE=/lost/key\x00\xff"
        env.write_bytes(original)
        result = self.run_setup("--init-env")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Recover its original key", result.stderr)
        self.assertEqual(env.read_bytes(), original)
        self.assertFalse(self.output.exists())

    def test_existing_env_untouched_without_init(self):
        self.require_openssl()
        env = self.repo / ".env"
        env.write_bytes(b"existing literal configuration\xff")
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(env.read_bytes(), b"existing literal configuration\xff")

    def test_repeat_never_overwrites(self):
        self.require_openssl()
        self.assertEqual(self.run_setup().returncode, 0)
        key = self.output / "signing-key.pem"
        public = self.output / "authorized-public.sec1"
        # Compare identity by public bytes and inode; Python never reads private bytes.
        before = (key.stat().st_ino, key.stat().st_mtime_ns, public.read_bytes())
        result = self.run_setup("--init-env")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("identity already exists", result.stderr)
        self.assertEqual(before, (key.stat().st_ino, key.stat().st_mtime_ns, public.read_bytes()))
        self.assertFalse((self.repo / ".env").exists())

    def test_unsafe_directories_and_collisions(self):
        unsafe = self.base / "unsafe"
        unsafe.mkdir(mode=0o755)
        unsafe.chmod(0o755)
        link = self.base / "alias"
        link.symlink_to(unsafe, target_is_directory=True)
        safe = self.base / "safe"
        safe.mkdir(mode=0o700)
        collision = safe / "signing-key.pem"
        collision.write_bytes(b"do not overwrite")
        dangling = self.base / "dangling"
        dangling.symlink_to(self.base / "missing")
        for path in (unsafe, link, link / "child", dangling, self.repo / "keys", safe):
            with self.subTest(path=path):
                self.assertNotEqual(self.run_setup("--out-dir", path).returncode, 0)
        self.assertEqual(stat.S_IMODE(unsafe.stat().st_mode), 0o755)
        self.assertEqual(collision.read_bytes(), b"do not overwrite")
        self.assertFalse((self.repo / "keys").exists())
        collision.unlink()
        collision.symlink_to(self.base / "missing")
        self.assertNotEqual(self.run_setup("--out-dir", safe).returncode, 0)
        self.assertTrue(collision.is_symlink())

    def test_env_symlink_preserved(self):
        env = self.repo / ".env"
        env.symlink_to(self.base / "lost-env")
        self.assertNotEqual(self.run_setup("--init-env").returncode, 0)
        self.assertTrue(env.is_symlink())
        self.assertFalse(self.output.exists())

    def test_missing_openssl_and_generation_failure_cleanup(self):
        binary = self.base / "bin"
        binary.mkdir()
        env = dict(os.environ, PATH=str(binary))
        result = self.run_setup(env=env)
        self.assertIn("OpenSSL is required", result.stderr)
        self.assertFalse(self.output.exists())
        fake = binary / "openssl"
        fake.write_text("#!/bin/sh\necho secret-diagnostic >&2\nexit 17\n")
        fake.chmod(0o700)
        result = self.run_setup(env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("secret-diagnostic", result.stdout + result.stderr)
        self.assertFalse(self.output.exists())
        self.output.mkdir(mode=0o700)
        sentinel = self.output / "unrelated"
        sentinel.write_bytes(b"keep")
        self.assertNotEqual(self.run_setup(env=env).returncode, 0)
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_partial_publication_failure_cleans_only_owned_files(self):
        self.require_openssl()
        self.output.mkdir(mode=0o700)
        sentinel = self.output / "unrelated"
        sentinel.write_bytes(b"keep")
        module = runpy.run_path(str(self.script))
        real_link = os.link

        def fail_public(source, destination):
            if Path(destination).name == "authorized-public.sec1":
                raise OSError("injected public publication failure")
            return real_link(source, destination)

        with mock.patch("os.link", side_effect=fail_public):
            self.assertEqual(module["main"]([]), 2)
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_config_failure_keeps_published_identity(self):
        self.require_openssl()
        module = runpy.run_path(str(self.script))
        real_link = os.link

        def fail_env(source, destination):
            if Path(destination) == self.repo / ".env":
                raise OSError("injected config publication failure")
            return real_link(source, destination)

        with mock.patch("os.link", side_effect=fail_env):
            self.assertEqual(module["main"](["--init-env"]), 2)
        self.assertTrue((self.output / "signing-key.pem").is_file())
        self.assertTrue((self.output / "authorized-public.sec1").is_file())
        self.assertFalse((self.repo / ".env").exists())
        self.assertFalse(list(self.output.glob(".setup-*")))

    def test_unicode_line_boundaries_refused_before_generation(self):
        for separator in ("\x85", "\u2028", "\u2029"):
            with self.subTest(separator=repr(separator)):
                output = self.base / ("identity" + separator + "split")
                result = self.run_setup("--out-dir", output, "--init-env")
                self.assertEqual(result.returncode, 2)
                self.assertIn("cannot be represented", result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse((self.repo / ".env").exists())

    def test_help_is_read_only(self):
        result = self.run_setup("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--out-dir", result.stdout)
        self.assertFalse(self.output.exists())
        self.assertFalse((self.repo / ".env").exists())


if __name__ == "__main__":
    unittest.main()
