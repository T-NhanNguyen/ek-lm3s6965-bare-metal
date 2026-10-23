"""OFFLINE flash-wrapper regression; only a refusing fake OpenOCD is reachable.

Run: python3 -B tests/boot/test_ab_flash_script.py
Requires local bash, python3, Tcl 8.6 tclsh, dirname, mktemp and rm. No keys,
network, device access, signing or real OpenOCD; all fixtures are synthetic.
"""
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/ab-boot-provision-recovery.sh"


def artifact(slot="A", size=236):
    """Public unsigned fixture: the wrapper validates structure, not signatures."""
    base = {"A": 0x8800, "B": 0x24000}[slot]
    payload = struct.pack("<II", 0x20010000, base | 1) + bytes(size - 8)
    return (b"LM3S6965-FW-V1\0\0"
            + struct.pack("<8I", 1, 160, 1, "AB".index(slot), base, size, 42, 0)
            + hashlib.sha256(payload).digest() + bytes(80) + payload)


def parse_commands(tclsh, commands):
    """Evaluate only in a safe Tcl interpreter with inert recording aliases.

    The outer interpreter receives hex data, never interpolated Tcl source.
    Even substituted command names are inert; extra calls fail exact comparison.
    """
    encoded = " ".join(command.encode("utf-8").hex() for command in commands)
    source = r'''
interp create -safe sandbox
set calls {}
proc record {name args} {
    lappend ::calls [linsert $args 0 $name]
    return ""
}
foreach name {init halt flash dump_image shutdown} {
    interp alias sandbox $name {} record $name
}
if {[catch {
    foreach command {ENCODED} {
        interp eval sandbox [encoding convertfrom utf-8 [binary decode hex $command]]
    }
} problem]} {
    puts stderr $problem
    exit 1
}
foreach call $calls {
    set words {}
    foreach word $call {
        lappend words [binary encode hex [encoding convertto utf-8 $word]]
    }
    puts [join $words " "]
}
'''.replace("ENCODED", encoded)
    result = subprocess.run([tclsh], input=source, text=True, capture_output=True,
                            timeout=5, check=True)
    return [[bytes.fromhex(word).decode("utf-8") for word in line.split()]
            for line in result.stdout.splitlines()]


class FlashScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = {}
        for name in ("bash", "python3", "tclsh", "dirname", "mktemp", "rm", "cmp"):
            path = shutil.which(name)
            if path is None:
                raise RuntimeError("offline regression prerequisite missing: " + name)
            cls.tools[name] = path

    def invoke(self, directory, slot, package, component, args=None, fake_type="refuse"):
        directory = Path(directory)
        binary = directory / "bin"
        binary.mkdir()
        # Deliberately exclude the host PATH: no actual OpenOCD fallback exists.
        for name in ("python3", "dirname", "mktemp", "rm", "cmp"):
            (binary / name).symlink_to(self.tools[name])
        record = directory / "record.jsonl"
        recorder = directory / "recorder.py"
        if fake_type == "refuse":
            recorder.write_text('''import json
import os
import sys
with open(os.environ["FLASH_RECORD"], "a") as output:
    output.write(json.dumps(sys.argv[1:]) + "\\n")
sys.exit(73)  # Refuse every operation, including any attempted reset.
''')
        elif fake_type == "success":
            recorder.write_text('''import json
import os
import shutil
import sys
with open(os.environ["FLASH_RECORD"], "a") as output:
    output.write(json.dumps(sys.argv[1:]) + "\\n")
tmp = os.environ.get("TMPDIR")
if tmp:
    src_payload = os.path.join(tmp, "payload.bin")
    src_manifest = os.path.join(tmp, "manifest.bin")
    dst_payload = os.path.join(tmp, "readback-payload.bin")
    dst_manifest = os.path.join(tmp, "readback-manifest.bin")
    if os.path.exists(src_payload):
        shutil.copy(src_payload, dst_payload)
    if os.path.exists(src_manifest):
        shutil.copy(src_manifest, dst_manifest)
''')
        elif fake_type == "bad_readback":
            recorder.write_text('''import json
import os
import sys
with open(os.environ["FLASH_RECORD"], "a") as output:
    output.write(json.dumps(sys.argv[1:]) + "\\n")
# Create empty readback files to force a mismatch.
tmp = os.environ.get("TMPDIR")
if tmp:
    open(os.path.join(tmp, "readback-payload.bin"), "wb").close()
    open(os.path.join(tmp, "readback-manifest.bin"), "wb").close()
''')
        fake = binary / "openocd"
        fake.write_text("#!/bin/sh\nexec " + shlex.quote(sys.executable) + " "
                        + shlex.quote(str(recorder)) + ' "$@"\n')
        fake.chmod(0o755)
        temporary = directory / component
        temporary.mkdir()
        pkg = directory / "public fixture [not Tcl]; $.pkg"
        pkg.write_bytes(package)
        cmd = [self.tools["bash"], str(SCRIPT)]
        if args is not None:
            cmd.extend(args)
        else:
            cmd.extend([slot, str(pkg)])
        result = subprocess.run(
            cmd, cwd=ROOT,
            env={"PATH": str(binary), "TMPDIR": str(temporary),
                 "FLASH_RECORD": str(record), "LC_ALL": "C"},
            capture_output=True, text=True, timeout=10)
        calls = [json.loads(line) for line in record.read_text().splitlines()] if record.exists() else []
        # EXIT cleanup must still handle the original literal path.
        self.assertEqual(list(temporary.iterdir()), [])
        return result, calls, temporary

    def test_literal_paths_and_unchanged_flash_ranges(self):
        components = ("spaces in temporary parent",
                      'Tcl [error INJECTED] $missing ${missing}; unmatched } { " '
                      "backslash\\ and backslash-newline\\\n next\r\ttab café")
        for slot in ("A", "B"):
            for size in (236, 111616):
                for component in components:
                    with self.subTest(slot=slot, size=size, component=component):
                        with tempfile.TemporaryDirectory(prefix="ab-flash-offline-") as directory:
                            result, calls, temporary = self.invoke(directory, slot, artifact(slot, size), component)
                            self.assertEqual(result.returncode, 1, result.stderr)
                            self.assertIn("openocd erase/write/dump failed", result.stderr)
                            self.assertEqual(len(calls), 1, "must not reset after tool refusal")
                            args = calls[0]
                            self.assertEqual(args[:2], ["-f", str(ROOT / "tools/openocd/board/ek-lm3s6965.cfg")])
                            self.assertEqual(args[2::2], ["-c"] * 9)
                            parsed = parse_commands(self.tools["tclsh"], args[3::2])
                            self.assertEqual(len(parsed), 9)
                            path = Path(parsed[4][3])
                            self.assertEqual(path.name, "payload.bin")
                            self.assertEqual(path.parent.parent, temporary)
                            self.assertTrue(path.parent.name.startswith("ab-boot-flash."))
                            base, manifest = {"A": (0x8800, 0x23C00), "B": (0x24000, 0x3F400)}[slot]
                            self.assertEqual(parsed, [
                                ["init"], ["halt"],
                                ["flash", "erase_sector", "0", str(base // 1024), str((base + size - 1) // 1024)],
                                ["flash", "erase_sector", "0", str(manifest // 1024), str(manifest // 1024)],
                                ["flash", "write_bank", "0", str(path), f"0x{base:X}"],
                                ["flash", "write_bank", "0", str(path.parent / "manifest.bin"), f"0x{manifest:X}"],
                                ["dump_image", str(path.parent / "readback-payload.bin"), f"0x{base:X}", str(size)],
                                ["dump_image", str(path.parent / "readback-manifest.bin"), f"0x{manifest:X}", "160"],
                                ["shutdown"],
                            ])

    def test_parser_rejects_unescaped_substitution(self):
        with self.assertRaises(subprocess.CalledProcessError):
            parse_commands(self.tools["tclsh"], ["dump_image /tmp/[error INJECTED]/file 0x8800 236"])

    def test_failed_validation_never_invokes_tool(self):
        valid = artifact()
        cases = [("truncated", "A", valid[:159]),
                 ("trailing bytes", "A", valid + b"x"),
                 ("slot mismatch", "B", valid)]
        for offset, label in ((0, "magic"), (16, "format"), (20, "manifest size"),
                              (24, "product"), (32, "base"), (36, "length"),
                              (44, "flags"), (48, "digest"), (80, "reserved")):
            changed = bytearray(valid)
            changed[offset] ^= 1
            cases.append((label, "A", bytes(changed)))
        # Rehash invalid vectors so vector checks, not the digest check, reject.
        for offset, value, label in ((160, 0x2000FFF8, "MSP"),
                                     (164, 0x8800, "Thumb bit"),
                                     (164, 0x8800 + 237, "reset bound")):
            changed = bytearray(valid)
            struct.pack_into("<I", changed, offset, value)
            changed[48:80] = hashlib.sha256(changed[160:]).digest()
            cases.append((label, "A", bytes(changed)))
        for label, slot, package in cases:
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory(prefix="ab-flash-invalid-") as directory:
                    result, calls, _ = self.invoke(directory, slot, package, "invalid [error INJECTED] $")
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("package validation failed", result.stderr)
                    self.assertEqual(calls, [], "invalid packages must not reach even the fake tool")

    def test_option_parse_malformed_args(self):
        cases = [
            ([], "expected exactly 2 arguments"),
            (["--no-reset"], "expected exactly 2 arguments"),
            (["--no-reset", "A"], "expected exactly 2 arguments"),
            (["--no-reset", "A", "pkg", "extra"], "expected exactly 2 arguments"),
            (["--unknown", "A"], "slot must be exactly A or B"),
        ]
        for args, expected in cases:
            with self.subTest(args=args):
                with tempfile.TemporaryDirectory(prefix="ab-flash-invalid-args-") as directory:
                    result, calls, _ = self.invoke(directory, "A", artifact("A"),
                                                   "missing [error INJECTED] $", args=args)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(expected, result.stderr)
                    self.assertEqual(calls, [])

    def test_no_reset_skips_reset_after_success(self):
        with tempfile.TemporaryDirectory(prefix="ab-flash-no-reset-success-") as directory:
            pkg = Path(directory) / "public fixture [not Tcl]; $.pkg"
            result, calls, _ = self.invoke(directory, "A", artifact("A"), "no-reset-success",
                                           args=["--no-reset", "A", str(pkg)], fake_type="success")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("target left halted (--no-reset)", result.stdout)
            self.assertEqual(len(calls), 1, "must not reset after --no-reset")
            self.assertIn("halt", calls[0])
            self.assertNotIn("reset run", calls[0])

    def test_default_resets_after_success(self):
        with tempfile.TemporaryDirectory(prefix="ab-flash-default-reset-success-") as directory:
            pkg = Path(directory) / "public fixture [not Tcl]; $.pkg"
            result, calls, _ = self.invoke(directory, "A", artifact("A"), "default-reset-success",
                                           args=["A", str(pkg)], fake_type="success")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("target reset", result.stdout)
            self.assertEqual(len(calls), 2, "must reset after verified write")
            self.assertIn("reset run", calls[1])
            self.assertNotIn("reset run", calls[0])

    def test_no_reset_readback_failure_does_not_reset(self):
        with tempfile.TemporaryDirectory(prefix="ab-flash-no-reset-fail-") as directory:
            pkg = Path(directory) / "public fixture [not Tcl]; $.pkg"
            result, calls, _ = self.invoke(directory, "A", artifact("A"), "no-reset-fail",
                                           args=["--no-reset", "A", str(pkg)], fake_type="bad_readback")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("readback mismatch", result.stderr)
            self.assertEqual(len(calls), 1, "must not reset after readback failure")
            self.assertNotIn("reset run", calls[0])


if __name__ == "__main__":
    unittest.main()
