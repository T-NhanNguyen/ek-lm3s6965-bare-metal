"""Offline configuration regression only; never launch product/tool processes."""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("host_integrity_runner", Path(__file__).with_name("runner.py"))
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

EXPECTED_CTEST = frozenset("""
boot_dependencies boot_allocator boot_receive boot_service boot_pc_workflow
boot_pc_ftp boot_startup boot_startup_target boot_handoff boot_handoff_target boot_watchdog boot_flash
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
        self.assertEqual(len(names), 37)
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
        self.assertEqual(len(registered), 37)
        self.assertEqual(set(registered), EXPECTED_CTEST)

    def test_selection_accounting_and_offline_ftp_intent(self):
        roster = [target + ":" + name for target, names in runner.ALL_CASES.items() for name in names]
        self.assertEqual(len(roster), 56)
        self.assertEqual(len(set(roster)), 56)
        self.assertEqual(runner.DATA["merged_cases"], {"ethernet:lwip": "ethernet:lwip-boot"})
        selected = [name for name in roster if name not in runner.DATA["merged_cases"]]
        self.assertEqual(len(selected), 55)
        self.assertIn("ctest:boot_pc_ftp", selected)
        self.assertIn("ctest:boot_resource_execution", selected)
        self.assertIn("ctest:boot_startup", selected)
        self.assertIn("ctest:boot_startup_target", selected)
        for rows in runner.DATA["targets"].values():
            for row in rows:
                self.assertEqual(len(row), 3)
                self.assertTrue(all(row))

    def test_startup_infrastructure_is_not_a_product_assertion(self):
        run = runner.Run.__new__(runner.Run)
        run.initial = {}
        run.native = Path("/unused-native")
        run.logpath = Path("/unused-log")
        run.record = mock.Mock()
        with mock.patch.object(runner, "identity", return_value={}):
            run.command = mock.Mock(return_value=(1, "STARTUP INFRASTRUCTURE: qemu infrastructure-timeout"))
            with self.assertRaisesRegex(runner.Invalid, "infrastructure failure"):
                run.case("ethernet:core")
            run.record.assert_not_called()
            run.command = mock.Mock(return_value=(1, "AssertionError: target oracle failed"))
            run.case("ethernet:core")
            self.assertEqual(run.record.call_args.args[1], "fail")

    def test_content_exclusion_boundary(self):
        for name in ("resume.tex", "work/resume.tex", ".dev-vault/secret", ".pi/local",
                     ".git/config", "out/build/a", "build-host/a", "x/__pycache__/a", "a.pyc"):
            with self.subTest(name=name):
                self.assertTrue(runner.identity_excluded(name))
        for name in ("tests/boot/test_resource_execution.py", "src/boot_receive.c",
                     "tests/harness/manifest.json", "docs/boot-reset-image.md"):
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


class StartupCommandEvidenceTests(unittest.TestCase):
    """Mocked commands only: no compiler, QEMU or product assertions execute."""

    @classmethod
    def setUpClass(cls):
        directory = runner.ROOT / "tests/boot"
        spec = importlib.util.spec_from_file_location("startup_target_evidence", directory / "test_startup_target.py")
        cls.target = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "path", [str(directory), *sys.path]):
            spec.loader.exec_module(cls.target)

    def test_exact_offline_qemu_output_oracle(self):
        import json
        warning = b'qemu-system-arm: warning: nic stellaris_enet.0 has no peer\n'
        # Exercise the real capture -> exit check -> exact oracle path with only
        # subprocess mocked. Both positive and pre-C rejection markers stay exact.
        for case in (0, 4):
            marker = b'STARTUP CLOCK REJECT PASS\n' if case == 4 else b'STARTUP PASS\n'
            scenarios = (
                ('plain', marker, 0, True),
                ('known-warning', warning + marker, 0, True),
                ('duplicated-warning', warning + warning + marker, 0, False),
                ('unknown-warning', b'qemu-system-arm: warning: unknown\n' + marker, 0, False),
                ('product-fail', warning + b'STARTUP FAIL\n', 0, False),
                ('nonzero-exit', warning + marker, 1, False),
                ('extra-output', warning + marker + b'extra\n', 0, False),
                ('trailing-warning', marker + warning, 0, False),
                ('wrong-marker', b'STARTUP PASS\n' if case == 4 else b'STARTUP CLOCK REJECT PASS\n', 0, False),
            )
            for name, raw, rc, accepted in scenarios:
                with self.subTest(case=case, scenario=name), tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary)
                    def complete(argv, **kwargs):
                        kwargs['stderr'].write(raw)
                        return subprocess.CompletedProcess(argv, rc)
                    with mock.patch.object(self.target.subprocess, 'run', side_effect=complete):
                        def capture_and_check():
                            output = self.target.run(['fake-qemu', '-nic', 'none'], directory, 'qemu')
                            self.target.check_qemu_output(output, case)
                        if accepted:
                            capture_and_check()
                        else:
                            with self.assertRaises((AssertionError, RuntimeError)):
                                capture_and_check()
                    self.assertEqual((directory / 'qemu.stdout').read_bytes(), b'')
                    self.assertEqual((directory / 'qemu.stderr').read_bytes(), raw)
                    self.assertEqual((directory / 'qemu.log').read_bytes(), raw)
                    evidence = json.loads((directory / 'qemu.json').read_text())
                    self.assertEqual(evidence['returncode'], rc)
                    self.assertEqual(evidence['stderr_bytes'], len(raw))
                    self.assertFalse(evidence['stderr_truncated'])

    def test_timeout_preserves_partial_bytes_and_prelaunch_command(self):
        import json
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            def timeout(argv, **kwargs):
                before = json.loads((directory / "qemu.json").read_text())
                self.assertEqual(before["argv"], ["fake-qemu"])
                self.assertEqual(before["status"], "started")
                self.assertIn("fake-qemu", (directory / "commands.log").read_text())
                kwargs["stdout"].write(b"partial stdout\xff")
                kwargs["stderr"].write(b"partial stderr\n")
                raise subprocess.TimeoutExpired(argv, kwargs["timeout"], output=b"partial stdout\xff", stderr=b"partial stderr\n")
            with mock.patch.object(self.target.subprocess, "run", side_effect=timeout):
                with self.assertRaisesRegex(RuntimeError, "STARTUP INFRASTRUCTURE:.*infrastructure-timeout") as caught:
                    self.target.run(["fake-qemu"], directory, "qemu", timeout=2)
            self.assertIsInstance(caught.exception.__cause__, subprocess.TimeoutExpired)
            result = json.loads((directory / "qemu.json").read_text())
            self.assertTrue(result["timed_out"])
            self.assertIsNone(result["returncode"])
            self.assertEqual((directory / "qemu.stdout").read_bytes(), b"partial stdout\xff")
            self.assertEqual((directory / "qemu.stderr").read_bytes(), b"partial stderr\n")
            self.assertIn("partial stderr", (directory / "qemu.log").read_text())

    def test_launch_error_retains_command_and_empty_streams(self):
        import json
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with mock.patch.object(self.target.subprocess, "run", side_effect=FileNotFoundError("fake tool")):
                with self.assertRaisesRegex(RuntimeError, "STARTUP INFRASTRUCTURE:.*infrastructure-launch-error"):
                    self.target.run(["missing"], directory, "launch")
            result = json.loads((directory / "launch.json").read_text())
            self.assertEqual(result["status"], "infrastructure-launch-error")
            self.assertIsNone(result["returncode"])
            self.assertFalse(result["timed_out"])
            self.assertEqual((directory / "launch.stdout").read_bytes(), b"")
            self.assertEqual((directory / "launch.stderr").read_bytes(), b"")

    def test_returncode_evidence_and_bounded_overflow(self):
        import json
        for rc, size in ((0, 4), (1, 4), (0, 9)):
            with self.subTest(rc=rc, size=size), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                def complete(argv, **kwargs):
                    kwargs["stdout"].write(b"x" * size)
                    kwargs["stderr"].write(b"err")
                    return subprocess.CompletedProcess(argv, rc)
                with mock.patch.object(self.target, "OUTPUT_LIMIT", 8), \
                     mock.patch.object(self.target.subprocess, "run", side_effect=complete):
                    if size > 8:
                        with self.assertRaisesRegex(RuntimeError, "STARTUP INFRASTRUCTURE:"):
                            self.target.run(["fake"], directory, "command")
                    elif rc:
                        with self.assertRaisesRegex(RuntimeError, "command: xxxxerr"):
                            self.target.run(["fake"], directory, "command")
                    else:
                        self.assertEqual(self.target.run(["fake"], directory, "command"), "xxxxerr")
                result = json.loads((directory / "command.json").read_text())
                self.assertEqual(result["returncode"], rc)
                self.assertFalse(result["timed_out"])
                self.assertEqual(result["stdout_bytes"], size)
                self.assertEqual(result["stdout_truncated"], size > 8)
                self.assertLessEqual((directory / "command.stdout").stat().st_size, 8)


class StartupRegressionEvidenceTests(unittest.TestCase):
    """Four-blocker regression mocks only; no tools or product code execute."""

    @classmethod
    def setUpClass(cls):
        cls.modules = {}
        directory = runner.ROOT / 'tests/boot'
        for name in ('test_handoff_target', 'test_diag_publication_target', 'test_resource_execution'):
            spec = importlib.util.spec_from_file_location(name + '_regression', directory / (name + '.py'))
            module = importlib.util.module_from_spec(spec)
            with mock.patch.object(sys, 'path', [str(directory), *sys.path]):
                spec.loader.exec_module(module)
            cls.modules[name] = module

    def test_handoff_publication_exact_optional_warning_and_exit(self):
        warning = 'qemu-system-arm: warning: nic stellaris_enet.0 has no peer\n'
        for name, marker in (('test_handoff_target', 'HANDOFF PASS\n'),
                             ('test_diag_publication_target', 'PUBLICATION PASS\n')):
            target = self.modules[name]
            for raw, code, accepted in (
                (marker, 0, True), (warning + marker, 0, True),
                (warning + warning + marker, 0, False),
                ('qemu-system-arm: warning: unknown\n' + marker, 0, False),
                (marker + warning, 0, False), (marker + 'extra\n', 0, False),
                (marker.rstrip('\n'), 0, False), ('STARTUP PASS\n', 0, False),
                (warning + marker, 1, False), (marker, -6, False)):
                with self.subTest(target=name, raw=raw, code=code):
                    if accepted:
                        target.check_pass_output(raw, marker, code)
                    else:
                        with self.assertRaises(RuntimeError):
                            target.check_pass_output(raw, marker, code)

    def test_resource_capture_exact_parser_and_calibration_exit(self):
        import json
        target = self.modules['test_resource_execution']
        warning = target.NIC_WARNING
        for case in (0, 1):
            values = dict.fromkeys(sorted(target.FIELDS), 0)
            values.update(test=case, status=case, stack=1024, before=476,
                          guards=1, output=1, msp=0x2000fe00, capacity=50000)
            marker = 'RESOURCE ' + ' '.join(f'{k}={v}' for k, v in values.items()) + '\n'
            for raw, code, accepted in (
                (marker, case, True), (warning + marker, case, True),
                (warning + warning + marker, case, False),
                ('unknown warning\n' + marker, case, False),
                (marker + warning, case, False), (marker + marker, case, False),
                (marker + 'extra\n', case, False), (marker.rstrip('\n'), case, False),
                (warning + marker, 1 - case, False), (marker, -6, False)):
                with self.subTest(case=case, raw=raw, code=code), tempfile.TemporaryDirectory() as temporary:
                    evidence = target.CommandEvidence(Path(temporary))
                    def complete(argv, **kwargs):
                        kwargs['stderr'].write(raw.encode())
                        return subprocess.CompletedProcess(argv, code)
                    with mock.patch.object(target.subprocess, 'run', side_effect=complete):
                        if accepted:
                            result = target.execute('fake-qemu', Path('fixture'), case, False, evidence)
                            self.assertEqual(result['test'], case)
                        else:
                            with self.assertRaises(ValueError):
                                target.execute('fake-qemu', Path('fixture'), case, False, evidence)
                    stem = Path(temporary) / f'001-fixture-case-{case}'
                    self.assertEqual(stem.with_suffix('.stdout').read_bytes(), b'')
                    self.assertEqual(stem.with_suffix('.stderr').read_bytes(), raw.encode())
                    record = json.loads(stem.with_suffix('.json').read_text())
                    self.assertEqual(record['returncode'], code)
                    self.assertEqual(record['stderr_bytes'], len(raw.encode()))
                    self.assertFalse(record['stderr_truncated'])

    def test_resource_timeout_launch_and_overflow_retain_evidence(self):
        import json
        target = self.modules['test_resource_execution']
        evidence_module = sys.modules[target.CommandEvidence.__module__]
        for fault in ('timeout', 'launch', 'overflow'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                evidence = target.CommandEvidence(Path(temporary))
                def complete(argv, **kwargs):
                    before = json.loads((Path(temporary) / '001-fault.json').read_text())
                    self.assertEqual(before['status'], 'started')
                    self.assertEqual(before['argv'], ['fake'])
                    if fault == 'launch':
                        raise FileNotFoundError('fake')
                    kwargs['stdout'].write(b'partial\xff')
                    kwargs['stderr'].write(b'err')
                    if fault == 'timeout':
                        raise subprocess.TimeoutExpired(argv, kwargs['timeout'])
                    return subprocess.CompletedProcess(argv, 0)
                with mock.patch.object(target.subprocess, 'run', side_effect=complete), \
                     mock.patch.object(evidence_module, 'OUTPUT_LIMIT', 7):
                    with self.assertRaises(ValueError):
                        evidence.run(['fake'], 'fault')
                record = json.loads((Path(temporary) / '001-fault.json').read_text())
                self.assertEqual(record['timed_out'], fault == 'timeout')
                self.assertEqual(record['returncode'], 0 if fault == 'overflow' else None)
                self.assertEqual(record['stdout_truncated'], fault != 'launch')
                self.assertEqual((Path(temporary) / '001-fault.stdout').read_bytes(),
                                 b'' if fault == 'launch' else b'partial')
                self.assertEqual((Path(temporary) / '001-fault.stderr').read_bytes(),
                                 b'' if fault == 'launch' else b'err')

    def test_resource_stack_inputs_survive_first_inspection_failure(self):
        target = self.modules['test_resource_execution']
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'build'
            build.mkdir()
            raw = b'fixture.c:1:1:frame\t512\tstatic\n'
            (build / 'frame.su').write_bytes(raw)
            (build / 'resource_probe').write_bytes(b'not an ELF')
            (build / 'resource_probe.map').write_text('map sentinel\n')
            evidence = target.CommandEvidence(Path(temporary) / 'evidence')
            with mock.patch.object(target, 'inspect', side_effect=ValueError('bad ELF')):
                with self.assertRaisesRegex(ValueError, 'bad ELF'):
                    target.gate(build, 'fake-qemu', 'fake-gcc', evidence)
            self.assertEqual((evidence.directory / 'inputs/frame.su').read_bytes(), raw)
            self.assertEqual((evidence.directory / 'inputs/resource_probe.map').read_text(),
                             'map sentinel\n')
            self.assertFalse((evidence.directory / 'measurements.json').exists())


if __name__ == "__main__":
    unittest.main()
