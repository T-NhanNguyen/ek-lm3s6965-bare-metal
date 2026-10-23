"""Offline local-build contract: fake CMake wrapper calls and real path policy.

No ARM builds, signatures, hardware, network or real private keys. The isolated
configure fixture copies the real trust module and PUBLIC validator unchanged;
malformed PUBLIC bytes distinguish reaching validation from a path rejection.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/build_ab_boot_demo"
PRESET = "ab-boot-demo"
GUARD_ERROR = "external build directory or the source/build subtree"
VALIDATION_ERROR = "expected exactly 65 raw SEC1 uncompressed P-256 PUBLIC bytes"


class PresetTests(unittest.TestCase):
    def test_preset_contract(self):
        presets = json.loads((ROOT / "CMakePresets.json").read_text())
        self.assertEqual(presets["version"], 2)
        self.assertEqual(presets["cmakeMinimumRequired"],
                         {"major": 3, "minor": 20, "patch": 0})
        configure, = presets["configurePresets"]
        self.assertEqual(configure["name"], PRESET)
        self.assertEqual(configure["generator"], "Unix Makefiles")
        self.assertEqual(configure["binaryDir"], "${sourceDir}/build")
        self.assertEqual(configure["cacheVariables"], {
            "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/cmake/toolchain-lm3s6965.cmake",
            "CMAKE_BUILD_TYPE": "Release",
            "LM3S6965_BUILD_AB_BOOT_DEMO": "ON",
            "LM3S6965_BUILD_AB_BOOT_DEMO_FTP": "ON",
            "LM3S6965_BUILD_BOOT_DISPATCH_IMAGE": "ON",
            "LM3S6965_BUILD_BOOT_RESET": "OFF",
            "LM3S6965_BOOT_PHYSICAL_PRODUCTION": "ON",
            "LM3S6965_BOOT_PUBLIC_KEY_FILE": "",
        })
        build, = presets["buildPresets"]
        self.assertEqual(build["name"], PRESET)
        self.assertEqual(build["configurePreset"], PRESET)
        self.assertEqual(build["targets"], ["lm3s6965_ab_ftp_a", "lm3s6965_ab_ftp_b"])


class WrapperTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ab-build-wrapper-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        # Never invoke the repository wrapper in place: its real .env is private.
        self.repo = self.directory / "fake repo"
        (self.repo / "scripts").mkdir(parents=True)
        self.script = self.repo / "scripts/build_ab_boot_demo"
        shutil.copy2(SCRIPT, self.script)
        self.dotenv = self.repo / ".env"
        self.caller = self.directory / "foreign cwd"
        self.caller.mkdir()
        self.key = self.caller / "public fixture.sec1"
        self.key.write_bytes(b"malformed PUBLIC fixture; fake CMake never reads it")
        self.private = self.directory / "fake-private.pem"
        self.private.write_text("fake fixture; stub packager does not read keys")
        self.config = (f"LM3S6965_BOOT_PRIVATE_KEY_FILE={self.private}\n"
                       "LM3S6965_BOOT_RELEASE_A=12\nLM3S6965_BOOT_RELEASE_B=34\n")
        self.dotenv.write_text(self.config + f"LM3S6965_BOOT_PUBLIC_KEY_FILE={self.key}\n")
        self.final = self.repo / "build/ab-demo-packages/release-12-34"
        self.package_record = self.directory / "packages.jsonl"
        package = self.repo / "tools/boot/package.py"
        package.parent.mkdir(parents=True)
        package.write_text('''import json, os, pathlib, sys
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
with open(os.environ["AB_PACKAGE_RECORD"], "a") as output:
    output.write(json.dumps({"tool": __file__, "args": args}) + "\\n")
assert pathlib.Path(args["--input"]).is_file()
assert args["--input"].endswith("lm3s6965_ab_ftp_" + args["--slot"].lower() + ".bin")
if os.environ.get("AB_SIGN_FAIL") == args["--slot"]:
    sys.exit(75)
with open(args["--output"], "x") as output:
    output.write("fake package " + args["--slot"])
''')
        binary = self.directory / "bin"
        binary.mkdir()
        self.record = self.directory / "calls.jsonl"
        fake = binary / "cmake"
        fake.write_text("#!" + sys.executable + "\n" + '''import json, os, sys
with open(os.environ["AB_BUILD_RECORD"], "a") as output:
    output.write(json.dumps({"argv": sys.argv[1:], "cwd": os.getcwd()}) + "\\n")
from pathlib import Path
if "--build" in sys.argv:
    for slot, directory in (("a", "slot-a-breathing"), ("b", "slot-b-blink")):
        output = Path("build/examples/ab-boot-demo") / directory / ("lm3s6965_ab_ftp_" + slot + ".bin")
        output.parent.mkdir(parents=True, exist_ok=True)
        if os.environ.get("AB_WRONG_BINARY_NAME"):
            output = output.with_name("non-ftp-" + slot + ".bin")
        output.write_bytes(b"fake binary")
sys.exit(int(os.environ.get("AB_BUILD_BUILD_EXIT" if "--build" in sys.argv else "AB_BUILD_CONFIGURE_EXIT", "0")))
''')
        fake.chmod(0o755)
        # Restrict PATH: accidental real CMake, signing or hardware tools cannot run.
        for name in ("bash", "dirname", "python3"):
            tool = shutil.which(name)
            if tool is None:
                raise RuntimeError("missing offline prerequisite: " + name)
            (binary / name).symlink_to(tool)
        self.env = dict(os.environ, PATH=str(binary), AB_BUILD_RECORD=str(self.record))
        self.env["AB_PACKAGE_RECORD"] = str(self.package_record)
        for name in ("PUBLIC_KEY_FILE", "PRIVATE_KEY_FILE", "RELEASE_A", "RELEASE_B"):
            self.env.pop("LM3S6965_BOOT_" + name, None)
        for name in ("AB_BUILD_CONFIGURE_EXIT", "AB_BUILD_BUILD_EXIT", "AB_SIGN_FAIL",
                     "AB_WRONG_BINARY_NAME"):
            self.env.pop(name, None)

    def write_config(self, text):
        self.dotenv.write_text(text + "\n" + self.config)

    def packages(self):
        return ([json.loads(line) for line in self.package_record.read_text().splitlines()]
                if self.package_record.exists() else [])

    def assert_unpublished(self):
        self.assertFalse(self.final.exists())
        self.assertEqual(list(self.final.parent.glob(".pair-*")), [])

    def invoke(self, *args):
        result = subprocess.run([str(self.script), *args], cwd=self.caller,
                                env=self.env, text=True, capture_output=True, timeout=10)
        calls = ([json.loads(line) for line in self.record.read_text().splitlines()]
                 if self.record.exists() else [])
        return result, calls

    def assert_calls(self, calls):
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(calls[0]["argv"]), 4)
        key_argument = calls[0]["argv"][2]
        prefix = "-DLM3S6965_BOOT_PUBLIC_KEY_FILE="
        self.assertTrue(key_argument.startswith(prefix))
        self.assertTrue(Path(key_argument[len(prefix):]).is_absolute())
        self.assertEqual(Path(key_argument[len(prefix):]).resolve(), self.key.resolve())
        self.assertEqual(calls, [
            {"argv": ["--preset", PRESET, key_argument, "-DLM3S6965_FTP_DIAGNOSTIC=OFF"],
             "cwd": str(self.repo.resolve())},
            {"argv": ["--build", "--preset", PRESET], "cwd": str(self.repo.resolve())},
        ])
        for package in self.packages()[-2:]:
            self.assertEqual(package["args"]["--public-key"], key_argument[len(prefix):])
            self.assertEqual(package["args"]["--private-key"], str(self.private))

    def test_explicit_caller_relative_key_overrides_environment_and_dotenv(self):
        self.env["LM3S6965_BOOT_PUBLIC_KEY_FILE"] = "/missing/env-key"
        self.write_config("LM3S6965_BOOT_PUBLIC_KEY_FILE=/missing/overridden-public")
        result, calls = self.invoke(self.key.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)

    def test_dotenv_absolute_key_ignores_inherited_environment(self):
        self.env["LM3S6965_BOOT_PUBLIC_KEY_FILE"] = "/missing/inherited-key"
        self.write_config("LM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.key) + "\n")
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)

    def test_dotenv_repo_relative_spaces_quotes_comments_and_literal_values(self):
        self.key = self.repo / "public space $HOME `literal` $(literal) # file.sec1"
        self.key.write_bytes(b"PUBLIC fixture")
        for quote in ("", "'", '"'):
            with self.subTest(quote=quote):
                self.record.unlink(missing_ok=True)
                if self.final.exists():
                    for output in self.final.iterdir():
                        output.unlink()
                    self.final.rmdir()
                self.write_config(
                    "\n  # PUBLIC path\n\tLM3S6965_BOOT_PUBLIC_KEY_FILE  =  "
                    + quote + self.key.name + quote + "  \n\n")
                result, calls = self.invoke()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_calls(calls)

    def test_absolute_key(self):
        result, calls = self.invoke(str(self.key))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)

    def test_missing_dotenv_has_no_environment_fallback(self):
        self.env["LM3S6965_BOOT_PUBLIC_KEY_FILE"] = str(self.key)
        self.dotenv.unlink()
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("repository .env:", result.stderr)
        self.assertEqual(calls, [])

    def test_invalid_dotenv_does_not_call_cmake(self):
        assignment = "LM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.key) + "\n"
        cases = {
            "empty file": "",
            "only comments": "# comment\n",
            "empty value": "LM3S6965_BOOT_PUBLIC_KEY_FILE=\n",
            "quoted empty": 'LM3S6965_BOOT_PUBLIC_KEY_FILE=""\n',
            "quoted whitespace": "LM3S6965_BOOT_PUBLIC_KEY_FILE='  '\n",
            "duplicate": assignment * 2,
            "unknown": assignment + "OTHER=value\n",
            "malformed": "export " + assignment,
            "no equals": "LM3S6965_BOOT_PUBLIC_KEY_FILE\n",
            "unmatched quote": "LM3S6965_BOOT_PUBLIC_KEY_FILE='missing\n",
            "mismatched quote": 'LM3S6965_BOOT_PUBLIC_KEY_FILE=\'missing"\n',
            "trailing shell": "LM3S6965_BOOT_PUBLIC_KEY_FILE='path'; command\n",
            "inline comment after quote": 'LM3S6965_BOOT_PUBLIC_KEY_FILE="'
            + str(self.key) + '" # no\n',
            "control": assignment + "\x00",
            "oversized": "#" * 4097,
            "missing public file": "LM3S6965_BOOT_PUBLIC_KEY_FILE=/missing/public\n",
            "directory public file": "LM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.repo),
        }
        for label, text in cases.items():
            with self.subTest(label=label):
                self.write_config(text)
                result, calls = self.invoke()
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertTrue(result.stderr)
                self.assertEqual(calls, [])
        self.dotenv.write_bytes(b"\xff")
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("repository .env:", result.stderr)
        self.assertEqual(calls, [])

    def test_dotenv_exact_size_limit(self):
        assignment = "LM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.key) + "\n"
        self.dotenv.write_text(assignment + self.config + "#" +
                               "x" * (4096 - len(assignment) - len(self.config) - 1))
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)

    def test_nonregular_dotenv_does_not_block_or_call_cmake(self):
        self.dotenv.unlink()
        self.dotenv.mkdir()
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("repository .env:", result.stderr)
        self.assertEqual(calls, [])
        self.dotenv.rmdir()
        os.mkfifo(self.dotenv)
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("regular file", result.stderr)
        self.assertEqual(calls, [])
        self.dotenv.unlink()
        self.dotenv.symlink_to(self.key)
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("repository .env:", result.stderr)
        self.assertEqual(calls, [])

    def test_shell_looking_data_is_never_executed(self):
        marker = self.directory / "must-not-exist"
        # Built-in printf works even with the restricted PATH if a shell is used.
        for payload in (f"$(printf executed > {marker})",
                        f"`printf executed > {marker}`",
                        f"ignored; printf executed > {marker}",
                        "$LM3S6965_BOOT_PUBLIC_KEY_FILE"):
            with self.subTest(payload=payload):
                self.write_config("LM3S6965_BOOT_PUBLIC_KEY_FILE=" + payload + "\n")
                result, calls = self.invoke()
                self.assertEqual(result.returncode, 2)
                self.assertIn("PUBLIC key must be", result.stderr)
                self.assertEqual(calls, [])
                self.assertFalse(marker.exists())

    def test_invalid_arguments_do_not_call_cmake(self):
        for args in (("missing.sec1",), (".",), ("",), ("a", "b")):
            with self.subTest(args=args):
                result, calls = self.invoke(*args)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(calls, [])

    def test_configure_failure_prevents_build(self):
        self.env["AB_BUILD_CONFIGURE_EXIT"] = "73"
        self.write_config("LM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.key))
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 73)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["argv"][:2], ["--preset", PRESET])
        self.assertEqual(self.packages(), [])
        self.assert_unpublished()

    def test_configure_forces_diagnostic_off(self):
        """The approved packaging build must always force diagnostics OFF."""
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0]["argv"][-1], "-DLM3S6965_FTP_DIAGNOSTIC=OFF")

    def test_exact_package_commands_and_published_pair(self):
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)
        packages = self.packages()
        self.assertEqual(len(packages), 2)
        for record, slot, directory, release in zip(
                packages, ("A", "B"), ("slot-a-breathing", "slot-b-blink"), ("12", "34")):
            self.assertEqual(record["tool"], str(self.repo / "tools/boot/package.py"))
            args = record["args"]
            output = Path(args["--output"])
            self.assertEqual(output.name, f"slot-{slot.lower()}.pkg")
            self.assertEqual(output.parent.parent, self.final.parent)
            self.assertEqual(args, {
                "--input": str(self.repo / "build/examples/ab-boot-demo" / directory /
                               f"lm3s6965_ab_ftp_{slot.lower()}.bin"),
                "--input-format": "bin", "--slot": slot, "--release": release,
                "--private-key": str(self.private), "--public-key": str(self.key),
                "--output": str(output),
            })
        self.assertEqual(Path(packages[0]["args"]["--output"]).parent,
                         Path(packages[1]["args"]["--output"]).parent)
        self.assertEqual(sorted(p.name for p in self.final.iterdir()),
                         ["slot-a.pkg", "slot-b.pkg"])
        import shlex
        command = shlex.split(result.stdout.strip().splitlines()[-1])
        self.assertEqual(command, [str(self.repo / "scripts/ab-boot-firmwareflash-ftp.sh"),
                                   "download", str(self.final)])

    def test_required_fields_and_uint32_fail_before_build_even_with_override(self):
        public = f"LM3S6965_BOOT_PUBLIC_KEY_FILE={self.key}\n"
        assignments = (public + self.config).splitlines(keepends=True)
        for line in assignments:
            name = line.split("=", 1)[0]
            for replacement in ("", name + "=\n"):
                with self.subTest(name=name, replacement=replacement):
                    self.dotenv.write_text("".join(replacement if item == line else item
                                                   for item in assignments))
                    result, calls = self.invoke(str(self.key))
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(name, result.stderr)
                    self.assertEqual(calls, [])
                    self.assertEqual(self.packages(), [])
        for slot in ("A", "B"):
            name = "LM3S6965_BOOT_RELEASE_" + slot
            for value in ("-1", "+1", "4294967296", "1.0", "0x10", "one", "1 2"):
                with self.subTest(name=name, value=value):
                    self.dotenv.write_text(public + self.config.replace(
                        name + ("=12" if slot == "A" else "=34"), name + "=" + value))
                    result, calls = self.invoke()
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(name, result.stderr)
                    self.assertEqual(calls, [])

    def test_private_path_policy_does_not_open_material(self):
        internal = self.repo / "private fixture"
        internal.write_text("fake only")
        alias = self.directory / "external alias"
        alias.symlink_to(internal)
        for path in ("relative.pem", str(internal), str(alias),
                     str(self.repo / "build/../private fixture")):
            with self.subTest(path=path):
                self.write_config(f"LM3S6965_BOOT_PUBLIC_KEY_FILE={self.key}")
                self.dotenv.write_text(self.dotenv.read_text().replace(str(self.private), path))
                result, calls = self.invoke()
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("LM3S6965_BOOT_PRIVATE_KEY_FILE", result.stderr)
                self.assertEqual(calls, [])

    def test_all_configuration_environment_values_are_ignored(self):
        for name in ("PUBLIC_KEY_FILE", "PRIVATE_KEY_FILE", "RELEASE_A", "RELEASE_B"):
            self.env["LM3S6965_BOOT_" + name] = "invalid inherited value"
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(calls)
        self.assertEqual([p["args"]["--release"] for p in self.packages()], ["12", "34"])
        self.assertTrue(all(p["args"]["--private-key"] == str(self.private)
                            for p in self.packages()))

    def test_build_failure_never_signs(self):
        self.env["AB_BUILD_BUILD_EXIT"] = "74"
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 74)
        self.assertEqual(len(calls), 2)
        self.assertEqual(self.packages(), [])
        self.assert_unpublished()

    def test_sign_failure_cleans_staging_and_preserves_other_release(self):
        old = self.final.parent / "release-1-2"
        old.mkdir(parents=True)
        sentinel = old / "slot-a.pkg"
        sentinel.write_text("old package")
        for slot, count in (("A", 1), ("B", 2)):
            with self.subTest(slot=slot):
                self.package_record.unlink(missing_ok=True)
                self.env["AB_SIGN_FAIL"] = slot
                result, _ = self.invoke()
                self.assertEqual(result.returncode, 75, result.stderr)
                self.assertEqual(len(self.packages()), count)
                self.assert_unpublished()
                self.assertEqual(sentinel.read_text(), "old package")

    def test_repeat_directory_refused_before_build_and_preserved(self):
        result, _ = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        before = {p.name: p.read_bytes() for p in self.final.iterdir()}
        self.record.unlink()
        self.package_record.unlink()
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("already exists", result.stderr)
        self.assertEqual(calls, [])
        self.assertEqual(self.packages(), [])
        self.assertEqual({p.name: p.read_bytes() for p in self.final.iterdir()}, before)

    def test_wrong_binary_filename_is_never_used_as_fallback(self):
        self.env["AB_WRONG_BINARY_NAME"] = "1"
        result, _ = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.packages()), 1)
        self.assert_unpublished()

    def test_duplicate_each_key_and_wrong_config_filename(self):
        original = self.dotenv.read_text()
        for assignment in original.splitlines():
            with self.subTest(assignment=assignment.split("=", 1)[0]):
                self.dotenv.write_text(original + assignment + "\n")
                result, calls = self.invoke(str(self.key))
                self.assertEqual(result.returncode, 2)
                self.assertIn("duplicate", result.stderr)
                self.assertEqual(calls, [])
        self.dotenv.rename(self.repo / ".env.wrong")
        result, calls = self.invoke(str(self.key))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])

    def test_uint32_boundaries_are_forwarded(self):
        self.dotenv.write_text(self.dotenv.read_text().replace("RELEASE_A=12", "RELEASE_A=0")
                               .replace("RELEASE_B=34", "RELEASE_B=4294967295"))
        result, _ = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([p["args"]["--release"] for p in self.packages()],
                         ["0", "4294967295"])
        self.assertTrue((self.final.parent / "release-0-4294967295").is_dir())

    def test_existing_directory_alias_is_not_overwritten(self):
        self.final.parent.mkdir(parents=True)
        self.final.symlink_to(self.directory / "nonexistent", target_is_directory=True)
        result, calls = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])
        self.assertTrue(self.final.is_symlink())


class PathPolicyTests(unittest.TestCase):
    def setUp(self):
        self.cmake = shutil.which("cmake")
        if self.cmake is None:
            raise RuntimeError("offline path-policy prerequisite missing: cmake")
        self.temporary = tempfile.TemporaryDirectory(prefix="ab-build-policy-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "source"
        (self.source / "cmake").mkdir(parents=True)
        (self.source / "scripts").mkdir()
        for relative in ("cmake/boot_trust_root.cmake", "scripts/validate-boot-public-key.py"):
            shutil.copyfile(ROOT / relative, self.source / relative)
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(path_policy NONE)\n'
            'include(cmake/boot_trust_root.cmake)\n')
        self.key = self.directory / "malformed-public.sec1"
        self.key.write_bytes(b"malformed PUBLIC fixture")

    def configure(self, binary, allowed):
        result = subprocess.run([
            self.cmake, "-S", str(self.source), "-B", str(binary),
            "-DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON",
            "-DLM3S6965_BOOT_PUBLIC_KEY_FILE=" + str(self.key),
            "-DPython3_EXECUTABLE=" + sys.executable,
        ], capture_output=True, text=True, timeout=30)
        # CMake may wrap diagnostics at different terminal widths.
        output = " ".join((result.stdout + result.stderr).split())
        self.assertNotEqual(result.returncode, 0, output)
        if allowed:
            self.assertNotIn(GUARD_ERROR, output)
            self.assertIn(VALIDATION_ERROR, output)
        else:
            self.assertIn(GUARD_ERROR, output)
            self.assertNotIn(VALIDATION_ERROR, output)
        self.assertFalse((binary / "generated/boot-public-root/"
                          "lm3s6965_boot_production_public_key.h").exists())

    def test_component_policy(self):
        cases = [(self.source, False), (self.source / "build", True),
                 (self.source / "build/nested", True),
                 (self.source / "build-lookalike", False),
                 (self.source / "other", False),
                 (self.source / "other/build", False),
                 (self.source / "build/../escaped", False),
                 (self.directory / "external", True),
                 (self.directory / "source-lookalike", True)]
        for binary, allowed in cases:
            with self.subTest(binary=binary):
                self.configure(binary, allowed)

    def test_symlink_policy(self):
        other = self.source / "other"
        other.mkdir()
        build = self.source / "build"
        build.mkdir()
        (build / "escape").symlink_to(other, target_is_directory=True)
        (self.directory / "external-alias").symlink_to(other, target_is_directory=True)
        (self.directory / "allowed-alias").symlink_to(build, target_is_directory=True)
        (self.directory / "root-alias").symlink_to(self.source, target_is_directory=True)
        for binary, allowed in ((build / "escape/nested", False),
                                (self.directory / "external-alias", False),
                                (self.directory / "allowed-alias/nested", True),
                                (self.directory / "root-alias", False)):
            with self.subTest(binary=binary):
                self.configure(binary, allowed)


if __name__ == "__main__":
    unittest.main()
