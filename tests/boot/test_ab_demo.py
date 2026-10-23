"""Offline HOST demo checks. Scripted transports only; no sockets or keys.

Run: python3 -B tests/boot/test_ab_demo.py
"""
import ftplib
import hashlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.boot import demo
from tools.boot.ftp_transport import FTPTransport, ProtocolError
from tools.boot.workflow import Artifact, KEYS


def artifact(slot):
    base = {"A": 0x8800, "B": 0x24000}[slot]
    payload = struct.pack("<II", 0x20010000, base | 1) + bytes(228)
    return (b"LM3S6965-FW-V1\0\0" + struct.pack("<8I", 1, 160, 1, "AB".index(slot),
            base, 236, 42, 0) + hashlib.sha256(payload).digest() + bytes(80) + payload)


def snapshot(**changes):
    values = dict.fromkeys(KEYS, "none")
    values.update(format="1", sequence="20", running_slot="A", hash="0" * 64,
                  release="1", confirmed_slot="A", permitted_slot="B", trial_id="0",
                  health="00000007", stage="4", fault="0", detail="0", reset_flags="00000000")
    values.update(changes)
    return "".join(f"{key}={values[key]}\n" for key in KEYS).encode("ascii")


def ready(slot="B"):
    return snapshot(running_slot="A" if slot == "B" else "B",
                    confirmed_slot="A" if slot == "B" else "B",
                    sequence="21", candidate_slot=slot, permitted_slot="none",
                    trial_id="21", trial_state="ready")


def trial(slot="B", **changes):
    values = dict(sequence="22", running_slot=slot, confirmed_slot="A" if slot == "B" else "B",
                  hash=Artifact.from_bytes(artifact(slot)).digest, release="42",
                  candidate_slot=slot, permitted_slot="none", trial_id="21", trial_state="trial")
    values.update(changes)
    return snapshot(**values)


def confirmed(slot="B", **changes):
    values = dict(sequence="23", confirmed_slot=slot, candidate_slot="none",
                  permitted_slot="A" if slot == "B" else "B", trial_state="none")
    values.update(changes)
    return trial(slot, **values)


def report(body, slot="B"):
    app = demo.APPLICATIONS[slot]
    return (f"format=1\napplication={app}\nmessage={app} booted successfully. Demo LEDs.\n\n".encode()
            + body)


def read(body):
    return [("connect", None), ("binary", None), ("retr:boot-status", body)]


def read_report(body):
    return [("connect", None), ("binary", None), ("retr:boot-report", body)]


class Script:
    def __init__(self, steps):
        self.steps = list(steps)
        self.calls = []
        self.closed = False

    def call(self, name, body=None):
        self.calls.append((name, body))
        if not self.steps:
            raise AssertionError("unexpected call: " + name)
        expected, result = self.steps.pop(0)
        if expected != name:
            raise AssertionError((expected, name))
        if isinstance(result, BaseException):
            raise result
        return result

    def connect(self):
        return self.call("connect")

    def binary(self):
        return self.call("binary")

    def retr(self, name):
        return self.call("retr:" + name)

    def stor(self, name, body):
        return self.call("stor:" + name, body)

    def close(self):
        self.closed = True


class Clock:
    def __init__(self):
        self.now = 0
        self.sleeps = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class DemoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        # Intentionally unrelated filenames: only metadata selects slot.
        (self.directory / "banana.pkg").write_bytes(artifact("A"))
        (self.directory / "apple.pkg").write_bytes(artifact("B"))
        self.clock = Clock()
        self.out, self.err = io.StringIO(), io.StringIO()

    def invoke(self, steps, command=None, sleep=None):
        script = Script(steps)
        factory = mock.Mock(return_value=script)
        code = demo.main(command or ["download", str(self.directory)],
                         transport_factory=factory, clock=self.clock,
                         sleep=sleep or self.clock.sleep, stdout=self.out, stderr=self.err)
        self.assertTrue(script.closed)
        self.assertEqual(script.steps, [])
        return code, script, factory

    def start(self, tail, slot="B", upload_reply=None):
        baseline = snapshot(running_slot="A" if slot == "B" else "B",
                            confirmed_slot="A" if slot == "B" else "B", permitted_slot=slot)
        return read(baseline) * 2 + [("stor:firmware", upload_reply)] + tail

    def test_status_old_mcu_never_requests_report(self):
        code, script, factory = self.invoke(read(snapshot()), ["status"])
        self.assertEqual(code, 0)
        self.assertEqual(self.out.getvalue(), "MCU status (authenticated running image identity):\n"
                         + snapshot().decode())
        factory.assert_called_once_with("192.168.7.2", 21, "anonymous", "bench@",
                                        call_timeout=30, operation_timeout=75, monotonic=self.clock)
        self.assertFalse(any(name == "retr:boot-report" for name, _ in script.calls))

    def test_status_default_call_timeout_is_30(self):
        """The host FTP demo default call timeout is 30 seconds (deadline stays 75)."""
        code, script, factory = self.invoke(read(snapshot()), ["status"])
        self.assertEqual(code, 0)
        factory.assert_called_once_with("192.168.7.2", 21, "anonymous", "bench@",
                                        call_timeout=30, operation_timeout=75, monotonic=self.clock)

    def test_directory_errors_are_offline(self):
        cases = ("missing", "duplicate", "invalid", "oversized", "nonregular")
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                (path / "a.pkg").write_bytes(artifact("A"))
                if case != "missing":
                    (path / "b.pkg").write_bytes(artifact("B"))
                if case == "duplicate":
                    (path / "extra.pkg").write_bytes(artifact("A"))
                elif case == "invalid":
                    (path / "b.pkg").write_bytes(bytes(396))
                elif case == "oversized":
                    (path / "b.pkg").write_bytes(bytes(111777))
                elif case == "nonregular":
                    (path / "dir.pkg").mkdir()
                factory = mock.Mock(side_effect=AssertionError("must remain offline"))
                code = demo.main(["download", directory], transport_factory=factory,
                                 stdout=self.out, stderr=self.err)
                self.assertEqual(code, 2)
                factory.assert_not_called()

    def test_ready_reset_reconnect_report_and_durable_confirmation_both_slots(self):
        for slot in "AB":
            with self.subTest(slot=slot):
                self.out.seek(0)
                self.out.truncate()
                tail = (read(ready(slot)) + [("connect", OSError("reset disconnect"))]
                        + read(trial(slot, health="00000003")) + read(trial(slot))
                        + read_report(report(trial(slot), slot)) + read(trial(slot))
                        + read(trial(slot)) + [("stor:confirm", OSError("lost reply"))]
                        + read(confirmed(slot)))
                code, script, _ = self.invoke(self.start(tail, slot, OSError("ambiguous upload")))
                self.assertEqual(code, 0)
                uploads = [body for name, body in script.calls if name == "stor:firmware"]
                self.assertEqual(uploads, [artifact(slot)])
                confirmation = [body for name, body in script.calls if name == "stor:confirm"]
                digest = Artifact.from_bytes(artifact(slot)).digest
                self.assertEqual(confirmation, [f"format=1\nslot={slot}\nhash={digest}\ntrial_id=21\n".encode()])
                text = self.out.getvalue()
                baseline = snapshot(running_slot="A" if slot == "B" else "B",
                                    confirmed_slot="A" if slot == "B" else "B", permitted_slot=slot)
                self.assertEqual(text,
                    "Initial MCU status (authenticated running image identity):\n" + baseline.decode()
                    + f"Selected package identity (MCU identity match pending): destination={slot} hash={digest} release=42\n"
                    + "Post-upload READY MCU status (old running image; package identity match pending execution):\n" + ready(slot).decode()
                    + "Press the MCU reset button.\n"
                    + f"Application report: {demo.APPLICATIONS[slot]} booted successfully. Demo LEDs.\n"
                    + f"SUCCESS: durable CONFIRMED slot={slot} hash={digest} release=42 trial_id=21\n")
                names = [name for name, _ in script.calls]
                self.assertLess(names.index("retr:boot-report"), names.index("stor:confirm"))

    def test_reset_wait_survives_past_all_operation_deadlines(self):
        def long_wait(seconds):
            self.clock.now += 10000
        tail = (read(ready()) + [("connect", OSError("reset"))]
                + read(confirmed()) + read_report(report(confirmed())) + read(confirmed()))
        code, _, _ = self.invoke(self.start(tail), sleep=long_wait)
        self.assertEqual(code, 0)
        self.assertGreater(self.clock.now, 75)

    def test_confirmation_rechecks_health_after_report(self):
        tail = (read(trial()) + read_report(report(trial())) + read(trial())
                + read(trial(health="00000003")))
        code, script, _ = self.invoke(self.start(tail))
        self.assertEqual(code, 2)
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_fresh_confirmed_can_resolve_without_confirmation_stor(self):
        tail = read(confirmed()) + read_report(report(confirmed())) + read(confirmed())
        code, script, _ = self.invoke(self.start(tail))
        self.assertEqual(code, 0)
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_confirmation_progress_between_reads_both_slots(self):
        for slot in "AB":
            for transition in ("poll", "report", "status", "preconfirm"):
                with self.subTest(slot=slot, transition=transition):
                    self.out.seek(0)
                    self.out.truncate()
                    polled = confirmed(slot) if transition == "poll" else trial(slot)
                    reported = confirmed(slot) if transition in ("poll", "report") else trial(slot)
                    current = trial(slot) if transition == "preconfirm" else confirmed(slot)
                    tail = (read(polled) + read_report(report(reported, slot)) + read(current))
                    if transition == "preconfirm":
                        tail += read(confirmed(slot))
                    code, script, _ = self.invoke(self.start(tail, slot))
                    self.assertEqual(code, 0)
                    self.assertIn(f"SUCCESS: durable CONFIRMED slot={slot}", self.out.getvalue())
                    self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_report_and_current_require_health_even_when_confirmed(self):
        for slot in "AB":
            for bad in ({"health": "00000003"}, {"health": "none"}, {"fault": "1"}):
                for source in ("report", "current", "preconfirm", "durable"):
                    with self.subTest(slot=slot, bad=bad, source=source):
                        self.out.seek(0)
                        self.out.truncate()
                        reported = confirmed(slot, **bad) if source == "report" else trial(slot)
                        current = confirmed(slot, **bad) if source == "current" else trial(slot)
                        tail = (read(trial(slot)) + read_report(report(reported, slot))
                                + read(current))
                        if source == "report":
                            # Keep roles monotonic so this isolates report health.
                            tail = (read(trial(slot)) + read_report(report(reported, slot))
                                    + read(confirmed(slot)))
                        elif source == "preconfirm":
                            tail += read(confirmed(slot, **bad))
                        elif source == "durable":
                            tail += (read(trial(slot)) + [("stor:confirm", None)]
                                     + read(confirmed(slot, **bad)))
                        code, script, _ = self.invoke(self.start(tail, slot))
                        self.assertEqual(code, 2)
                        self.assertNotIn("SUCCESS:", self.out.getvalue())
                        self.assertEqual(sum(name == "stor:confirm" for name, _ in script.calls),
                                         1 if source == "durable" else 0)

    def test_execution_roles_reject_contradictions_independent_of_sequence(self):
        for slot in "AB":
            peer = "A" if slot == "B" else "B"
            for body in (trial(slot, permitted_slot=peer),
                         trial(slot, confirmed_slot=slot),
                         confirmed(slot, permitted_slot="none"),
                         confirmed(slot, permitted_slot=slot),
                         confirmed(slot, candidate_slot=slot)):
                with self.subTest(slot=slot, body=body):
                    code, script, _ = self.invoke(self.start(read(body), slot))
                    self.assertEqual(code, 2)
                    self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))
            for reported, current in (
                (confirmed(slot, sequence="22"), confirmed(slot)),
                (confirmed(slot), confirmed(slot, sequence="22")),
                (confirmed(slot), trial(slot, sequence="24")),
                (trial(slot, permitted_slot=peer), trial(slot)),
                (trial(slot), confirmed(slot, permitted_slot="none")),
            ):
                with self.subTest(slot=slot, reported=reported, current=current):
                    tail = read(trial(slot)) + read_report(report(reported, slot)) + read(current)
                    code, script, _ = self.invoke(self.start(tail, slot))
                    self.assertEqual(code, 2)
                    self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_report_mismatch_never_confirms(self):
        for wrong in (report(trial(hash="1" * 64)), report(trial(trial_id="19")),
                      report(trial(), "A"), report(trial(sequence="21"))):
            with self.subTest(wrong=wrong):
                tail = read(trial()) + read_report(wrong) + read(trial())
                code, script, _ = self.invoke(self.start(tail))
                self.assertEqual(code, 2)
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))
                self.assertNotIn("SUCCESS:", self.out.getvalue())

    def test_fallback_after_ready_fails(self):
        code, script, _ = self.invoke(self.start(read(ready()) + read(snapshot())))
        self.assertEqual(code, 2)
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_malformed_status_fails_not_infinite_retry(self):
        code, script, _ = self.invoke(self.start(read(b"bad\n")))
        self.assertEqual(code, 2)
        self.assertEqual(self.clock.sleeps, [])
        self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)

    def test_ctrl_c_during_indefinite_reset_wait(self):
        def interrupt(seconds):
            self.clock.now += 10000  # Manual reset has no total deadline.
            raise KeyboardInterrupt
        code, script, _ = self.invoke(self.start(read(ready())), sleep=interrupt)
        self.assertEqual(code, 130)
        self.assertIn("do not reupload", self.err.getvalue())
        self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)

    def test_ambiguous_upload_times_out_without_retry(self):
        tail = [("connect", OSError("lost"))] * 76
        code, script, _ = self.invoke(self.start(tail, upload_reply=OSError("lost")))
        self.assertEqual(code, 2)
        self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)

    def test_failed_ready_retains_sanitized_upload_diagnostic_without_retry(self):
        from test_ftp_transport import Data, FTP, Factory
        cases = (
            ("STOR firmware", ftplib.error_perm("550 SECRET server user password"),
             "phase=STOR-preliminary category=reply-permanent reply=550", False),
            ("final", ftplib.error_perm("550 SECRET server user password"),
             "phase=final category=reply-permanent reply=550", True),
            ("final", EOFError("SECRET server user password"),
             "phase=final category=disconnect reply=none", True),
        )
        for step, error, expected, sent in cases:
            for unavailable in (False, True):
                with self.subTest(step=step, error=type(error).__name__, unavailable=unavailable):
                    self.out.seek(0)
                    self.out.truncate()
                    self.err.seek(0)
                    self.err.truncate()
                    ftp = FTP(Data())
                    transport = FTPTransport("127.0.0.1", 21, "private-user", "private-password",
                                             ftp_factory=Factory(ftp))
                    transport.connect()
                    ftp.fail[step] = error
                    try:
                        transport.stor("firmware", artifact("B"))
                    except OSError as failure:
                        upload_failure = failure
                    else:
                        self.fail("upload must fail")
                    self.assertEqual(bytes(ftp.data.sent), artifact("B") if sent else b"")
                    self.assertEqual(ftp.calls.count(("STOR firmware",)), 1)
                    # Later read failures must not replace the upload provenance.
                    tail = ([('connect', OSError("different private read failure"))] * 76
                            if unavailable else read(snapshot()) * 76)
                    code, script, _ = self.invoke(self.start(tail, upload_reply=upload_failure))
                    self.assertEqual(code, 2)
                    self.assertIn("upload diagnostic: " + expected, self.err.getvalue())
                    self.assertIn("no success established", self.err.getvalue())
                    if not unavailable:
                        self.assertIn("READY/trial not established", self.err.getvalue())
                    text = self.out.getvalue() + self.err.getvalue()
                    for secret in ("SECRET", "private-user", "private-password", "server user password",
                                   "different private read failure"):
                        self.assertNotIn(secret, text)
                    self.assertNotIn("SUCCESS:", text)
                    self.assertNotIn("Press the MCU reset", text)
                    self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)
                    self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_malformed_or_missing_report_fails_closed(self):
        for body in (b"", b"bad\n", report(trial()).replace(b"Demo LEDs.", b"bad\rtext")):
            with self.subTest(body=body):
                code, script, _ = self.invoke(self.start(read(trial()) + read_report(body)))
                self.assertEqual(code, 2)
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))
        code, script, _ = self.invoke(self.start(read(trial()) + read_report(ProtocolError("FTP protocol failure"))))
        self.assertEqual(code, 4)
        self.assertIn("MCU/report support", self.err.getvalue())

    def test_ready_requires_complete_fresh_coherent_authority(self):
        changes = (
            {"sequence": "19", "trial_id": "19"},
            {"sequence": "21", "trial_id": "20"},
            {"sequence": "21", "trial_id": "22"},
            {"hash": "none"}, {"release": "none"}, {"sequence": "none"},
            {"trial_id": "none"}, {"hash": "1" * 64},
            {"confirmed_slot": "B"}, {"permitted_slot": "B"},
        )
        for change in changes:
            with self.subTest(change=change):
                values = dict(sequence="21", candidate_slot="B", permitted_slot="none",
                              trial_id="21", trial_state="ready")
                values.update(change)
                code, script, _ = self.invoke(self.start(read(snapshot(**values))))
                self.assertEqual(code, 2)
                self.assertNotIn("Press the MCU reset", self.out.getvalue())
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_download_requires_initial_identity_and_unchanged_admission(self):
        code, script, _ = self.invoke(read(snapshot(hash="none")))
        self.assertEqual(code, 2)
        self.assertFalse(any(name == "stor:firmware" for name, _ in script.calls))
        steps = (read(snapshot()) + read(snapshot(hash="1" * 64))
                 + [("stor:firmware", None)])
        code, script, _ = self.invoke(steps)
        self.assertEqual(code, 2)
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_ready_pins_acceptance_trial_and_exact_executed_package(self):
        for change in ({"trial_id": "22"}, {"hash": "1" * 64}, {"release": "43"},
                       {"hash": "none"}, {"sequence": "20"}, {"sequence": "21"}, {"confirmed_slot": "B"}):
            with self.subTest(change=change):
                code, script, _ = self.invoke(self.start(read(ready()) + read(trial(**change))))
                self.assertEqual(code, 2)
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_report_allows_monotonic_diagnostics_and_confirmation_progress(self):
        progressed = trial(health="00000007", stage="5", detail="1", reset_flags="00000002")
        for reported, current, tail in (
            (trial(), progressed,
             read(progressed) + [("stor:confirm", None)] + read(confirmed())),
            (trial(), confirmed(), []),
            (confirmed(), confirmed(), []),
        ):
            with self.subTest(reported=reported, current=current):
                steps = read(trial()) + read_report(report(reported)) + read(current) + tail
                code, _, _ = self.invoke(self.start(steps))
                self.assertEqual(code, 0)

    def test_report_rejects_backwards_sequence_roles_and_independent_fault(self):
        cases = (
            (trial(sequence="21"), trial()),
            (trial(sequence="23"), trial(sequence="22")),
            (confirmed(), trial(sequence="24")),
            (trial(confirmed_slot="none"), trial()),
            (trial(fault="1"), trial()),
            (trial(), trial(fault="1")),
            (trial(hash="none"), trial()),
        )
        for reported, current in cases:
            with self.subTest(reported=reported, current=current):
                tail = read(trial()) + read_report(report(reported)) + read(current)
                code, script, _ = self.invoke(self.start(tail))
                self.assertEqual(code, 2)
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_preconfirm_disconnect_retries_only_definitely_unsent(self):
        tail = (read(ready()) + read(trial()) + read_report(report(trial())) + read(trial())
                + [("connect", OSError("temporary disconnect"))]
                + read(trial()) + read(trial()) + [("stor:confirm", OSError("ambiguous"))]
                + read(confirmed()))
        code, script, _ = self.invoke(self.start(tail))
        self.assertEqual(code, 0)
        self.assertEqual(sum(name == "stor:confirm" for name, _ in script.calls), 1)
        self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)

    def test_ambiguous_confirmation_never_repeated(self):
        tail = (read(trial()) + read_report(report(trial())) + read(trial())
                + read(trial()) + [("stor:confirm", OSError("lost reply"))]
                + read(trial()) * 75)
        code, script, _ = self.invoke(self.start(tail))
        self.assertEqual(code, 2)
        self.assertEqual(sum(name == "stor:confirm" for name, _ in script.calls), 1)
        self.assertEqual(sum(name == "stor:firmware" for name, _ in script.calls), 1)

    def test_preconfirm_retries_bounded_and_stale_read_never_sent(self):
        prefix = read(trial()) + read_report(report(trial())) + read(trial())
        # One-second sleeps reach the 75-second pre-confirm bound.
        tail = prefix + ([("connect", OSError("temporary"))] + read(trial())) * 75
        code, script, _ = self.invoke(self.start(tail))
        self.assertEqual(code, 2)
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))
        for body in (trial(sequence="21"), trial(trial_id="22"), trial(fault="1"),
                     trial(hash="none"), b"bad\n"):
            with self.subTest(body=body):
                code, script, _ = self.invoke(self.start(prefix + read(body)))
                self.assertEqual(code, 2)
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_malformed_status_after_ready_is_not_a_disconnect(self):
        code, script, _ = self.invoke(self.start(read(ready()) + read(b"bad\n")))
        self.assertEqual(code, 2)
        self.assertEqual(self.clock.sleeps, [1])
        self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_permanent_transport_errors_after_ready_fail_promptly_redacted(self):
        # Exercise production translation without a socket: both a malformed
        # control reply and an oversized RETR must remain permanent to the demo.
        from test_ftp_transport import Data, FTP, Factory
        for malformed in (False, True):
            with self.subTest(malformed=malformed):
                ftp = FTP(Data(b"x" * 1025))
                transport = FTPTransport("127.0.0.1", 21, "anonymous", "bench@",
                                         ftp_factory=Factory(ftp))
                transport.connect()
                if malformed:
                    ftp.voidcmd = mock.Mock(return_value="200?secret reply")
                try:
                    transport.retr("boot-status")
                except ProtocolError as error:
                    failure = error
                else:
                    self.fail("permanent protocol failure required")
                self.assertNotIn("secret", str(failure))
                code, script, _ = self.invoke(self.start(read(ready()) + [("connect", failure)]))
                self.assertEqual(code, 4)
                self.assertNotIn("secret", self.err.getvalue())
                self.assertFalse(any(name == "stor:confirm" for name, _ in script.calls))

    def test_report_parser_bounds_and_strict_prefix(self):
        good = report(trial())
        self.assertEqual(demo.boot_report(good)[0], "slot-b-blink")
        for body in (good + b"x", good.replace(b"format=1", b"format=2", 1),
                     good.replace(b"application=", b"extra=x\napplication=", 1),
                     good.replace(b"Demo LEDs.", b"\x7f"),
                     good.replace(b"Demo LEDs.", b"\x80"),
                     good.replace(b"Demo LEDs.", b""), bytes(2049)):
            with self.subTest(body=body), self.assertRaises((ValueError, UnicodeError)):
                demo.boot_report(body)

    def test_real_transfer_caps_with_offline_data_doubles(self):
        # Reuse the existing socket-free FTP/data doubles, not a real listener.
        from test_ftp_transport import Data, FTP, Factory
        for name, limit in (("boot-status", 1024), ("boot-report", 2048)):
            for extra in (0, 1):
                with self.subTest(name=name, extra=extra):
                    ftp = FTP(Data(b"x" * (limit + extra)))
                    transport = FTPTransport("127.0.0.1", 21, "anonymous", "bench@",
                                             ftp_factory=Factory(ftp))
                    transport.connect()
                    try:
                        if extra:
                            with self.assertRaises(OSError):
                                transport.retr(name)
                        else:
                            self.assertEqual(transport.retr(name), b"x" * limit)
                    finally:
                        transport.close()

    def test_retr_allowlist_and_separate_caps(self):
        transport = FTPTransport("127.0.0.1", 21, "anonymous", "bench@")
        with mock.patch.object(transport, "_transfer", return_value=b"body") as transfer:
            self.assertEqual(transport.retr("boot-report"), b"body")
            transfer.assert_called_with("RETR boot-report", retr_limit=2048)
            transport.retr("boot-status")
            transfer.assert_called_with("RETR boot-status", retr_limit=1024)
            with self.assertRaises(ValueError):
                transport.retr("other")
            self.assertEqual(transfer.call_count, 2)


if __name__ == "__main__":
    unittest.main()
