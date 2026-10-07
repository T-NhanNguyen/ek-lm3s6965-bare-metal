"""Actual embedded Python lifecycle tests. No devices or child processes."""

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import io
from pathlib import Path
import subprocess
import tempfile
import threading
import types
import unittest
from unittest.mock import Mock, patch

import ethernet_link_test as fixtures


SOURCE = fixtures.MODULE_PATH.with_name("icdi-console.sh").read_text()
SOURCE = SOURCE.split("<<'PYTHON'\n", 1)[1].rsplit("\nPYTHON", 1)[0]
FIRMWARE = (fixtures.HEADER + "Ethernet diagnostic bring-up complete. "
            "Link acceptance pending\n"
            + fixtures.sample(0) + fixtures.sample(1))
RAW = b"".join(b"\x03" + bytes([byte]) for byte in FIRMWARE.encode())
REAL_TEMPFILE = tempfile.TemporaryFile
REAL_PREAD = __import__("os").pread
REAL_EVENT = threading.Event
REAL_THREAD = threading.Thread


class Harness:
    def __init__(self, failures=(), live=False, late=False, mutate=False,
                 actual_reader=False, raw=False, concurrent=False,
                 delayed_bootstrap=False, exception_type=OSError,
                 seconds="1", clock_step=0.1):
        self.seconds = seconds
        self.clock_step = clock_step
        self.clock = 0.0
        self.failures = set(failures)
        self.calls = []
        self.live = live
        self.late = late
        self.mutate = mutate
        self.actual_reader = actual_reader
        self.raw = raw
        self.concurrent = concurrent
        self.delayed_bootstrap = delayed_bootstrap
        self.exception_type = exception_type
        self.state = {}
        self.device = Mock()
        for method in ("open", "purge_buffers", "set_bitmode", "set_baudrate"):
            getattr(self.device, method).side_effect = (
                lambda *a, name=method, **k: self.operation(name))
        self.device.close.side_effect = self.close
        self.device.read_data.side_effect = [
            RAW, exception_type("reader original")]
        self.probe = Mock(returncode=None)
        self.probe.poll.side_effect = lambda: self.operation("poll")
        self.probe.terminate.side_effect = lambda: self.operation("terminate")
        self.probe.kill.side_effect = lambda: self.operation("kill")
        self.probe.wait.side_effect = self.wait
        self.sleep_count = 0
        self.wait_count = 0
        self.read_count = 0
        self.stdout = io.StringIO()
        self.stderr = io.StringIO()
        self.spool = None

    def operation(self, name):
        self.calls.append(name)
        if name in self.failures:
            raise self.exception_type(name + " original")

    def close(self):
        self.operation("close")
        if self.mutate:
            self.state["captured"].clear()
            self.state["captured"].extend(b"\x03X")
        if "decode" in self.failures:
            def broken_decode(data):
                self.operation("decode")
            self.state["read_itm_stimulus_port_zero"] = broken_decode

    def wait(self, **kwargs):
        assert kwargs == {"timeout": 5}
        self.wait_count += 1
        name = "wait" if self.wait_count == 1 else "reap"
        self.operation(name)
        if name == "wait" and "timeout" in self.failures:
            raise subprocess.TimeoutExpired("mock", 5)
        self.probe.returncode = -15
        return -15

    def run(self):
        owner = self
        event = REAL_EVENT()
        waiting = REAL_EVENT()
        release = REAL_EVENT()
        reads = 0
        bootstrap_entered = REAL_EVENT()
        bootstrap_release = REAL_EVENT()
        workers = []

        def controlled_read(size):
            nonlocal reads
            reads += 1
            if reads == 1:
                return RAW[:-2]
            waiting.set()
            if not release.wait(timeout=2):
                raise OSError("controlled reader release timed out")
            return RAW[-2:]

        if self.concurrent:
            self.device.read_data.side_effect = controlled_read

        class Stop:
            def set(self):
                owner.operation("stop")
                event.set()

            def is_set(self):
                return event.is_set()

        event_count = 0

        def make_event():
            nonlocal event_count
            owner.operation("event")
            event_count += 1
            return Stop() if event_count == 1 else REAL_EVENT()

        class ReaderThread:
            def __init__(self, target, args, **kwargs):
                owner.operation("thread-construct")
                owner.state = target.__globals__
                self.target, self.args = target, args
                self.alive = False
                self.worker = None

            def start(self):
                owner.operation("start")
                self.alive = True
                if owner.concurrent:
                    self.worker = REAL_THREAD(target=self.target,
                                              args=self.args, daemon=True)
                    workers.append(self.worker)
                    self.worker.start()
                    assert waiting.wait(timeout=2)
                elif owner.actual_reader:
                    self.target(*self.args)
                    self.alive = False
                else:
                    owner.state["captured"].extend(
                        RAW[:-2] if owner.late else RAW)
                if "snapshot" in owner.failures:
                    def broken_snapshot(data):
                        owner.operation("snapshot")
                    owner.state["bytes"] = broken_snapshot
                owner.operation("start-after")

            def is_alive(self):
                owner.operation("alive")
                return (self.worker.is_alive() if self.worker
                        else self.alive)

            def join(self, **kwargs):
                assert kwargs == {"timeout": 2}
                owner.operation("join")
                if self.worker:
                    assert event.is_set()
                    release.set()
                    self.worker.join(timeout=2)
                if owner.late:
                    owner.state["captured"].extend(RAW[-2:])
                if not owner.live:
                    self.alive = False
                    if "start" not in owner.failures:
                        owner.state["reader_completed"].set()

        class DelayedBootstrapThread(REAL_THREAD):
            def __init__(self, target, **kwargs):
                owner.state = target.__globals__
                super().__init__(target=target, **kwargs)
                workers.append(self)
                self.startup_wait = self._started.wait

                def checked_snapshot(data):
                    if data is owner.state["captured"]:
                        owner.operation("snapshot")
                    return bytes(data)

                owner.state["bytes"] = checked_snapshot

                def interrupted_wait(timeout=None):
                    assert bootstrap_entered.wait(timeout=2)
                    raise owner.exception_type("start original")

                self._started.wait = interrupted_wait

            def _bootstrap(self):
                bootstrap_entered.set()
                # The helper must finish cleanup before publication resumes.
                bootstrap_release.wait()
                super()._bootstrap()

            def join(self, **kwargs):
                owner.operation("join")
                return super().join(**kwargs)

            def is_alive(self):
                owner.operation("alive")
                result = super().is_alive()
                owner.unpublished_status = result
                owner.completion_at_status = (
                    owner.state["reader_completed"].is_set())
                return result

        class Spool:
            def __init__(self):
                owner.operation("temp")
                self.file = REAL_TEMPFILE()
                owner.spool = self

            def fileno(self):
                owner.operation("fileno")
                return self.file.fileno()

            def close(self):
                # Even an injected close error uses a controlled local resource.
                self.file.close()
                owner.operation("spool-close")

        def launch(*args, **kwargs):
            owner.operation("launch")
            kwargs["stderr"].file.write(
                b"ICDI PROBE READY: init-reset-run\n")
            kwargs["stderr"].file.flush()
            return owner.probe

        def sleep(seconds):
            owner.sleep_count += 1
            owner.operation("sleep%d" % owner.sleep_count)

        def monotonic():
            result = owner.clock
            owner.clock += owner.clock_step
            return result

        def pread(*args):
            owner.read_count += 1
            owner.operation("pread%d" % owner.read_count)
            return REAL_PREAD(*args)

        class Output(io.StringIO):
            def write(self, text):
                if "startup-report" in owner.failures and text.startswith(
                        "Listening"):
                    owner.operation("startup-report")
                if "header" in owner.failures and text.startswith("Captured"):
                    owner.operation("header")
                if "report" in owner.failures and text.startswith(
                        "ICDI DECODED TEXT END"):
                    owner.operation("report")
                return super().write(text)

        self.stdout = Output()
        class Device:
            def __getattr__(self, name):
                return getattr(owner.device, name)

            @property
            def timeouts(self):
                return (1, 1)

            @timeouts.setter
            def timeouts(self, value):
                owner.operation("timeouts")

        ftdi = types.ModuleType("pyftdi.ftdi")
        ftdi.Ftdi = Mock()
        ftdi.Ftdi.side_effect = lambda: (
            self.operation("construct") or Device())
        ftdi.Ftdi.BitMode.RESET = 0
        with ExitStack() as stack:
            for target, replacement in (
                ("threading.Thread", DelayedBootstrapThread
                 if self.delayed_bootstrap else ReaderThread),
                ("threading.Event", make_event),
                ("time.sleep", sleep),
                ("time.monotonic", monotonic),
                ("subprocess.Popen", launch),
                ("tempfile.TemporaryFile", Spool),
                ("os.pread", pread),
            ):
                stack.enter_context(patch(target, replacement))
            stack.enter_context(patch.dict("sys.modules", {
                "pyftdi": types.ModuleType("pyftdi"), "pyftdi.ftdi": ftdi}))
            stack.enter_context(patch("sys.argv", [
                "-", "mock.cfg", "1000000", self.seconds,
                "true" if self.raw else "false", "bring-up complete"]))
            stack.enter_context(redirect_stdout(self.stdout))
            stack.enter_context(redirect_stderr(self.stderr))
            try:
                exec(compile(SOURCE, "actual-embedded-helper", "exec"), {})
            except SystemExit as result:
                self.status = result.code
            finally:
                # Always release native threads, including assertion failures.
                # Bypass injected stop/join failures and restore startup wait.
                event.set()
                release.set()
                bootstrap_release.set()
                for worker in workers:
                    if hasattr(worker, "startup_wait"):
                        worker._started.wait = worker.startup_wait
                    assert worker._started.wait(timeout=2)
                    REAL_THREAD.join(worker, timeout=2)
                    assert not REAL_THREAD.is_alive(worker)
                if self.spool and not self.spool.file.closed:
                    self.spool.file.close()
        self.output = self.stdout.getvalue() + self.stderr.getvalue()
        return self


class LifecycleTests(unittest.TestCase):
    def test_direct_helper_invalid_durations_acquire_nothing(self):
        for seconds in ("nan", "+inf", "-inf", "inf", "0", "-1",
                        "malformed", "1e309"):
            with self.subTest(seconds=seconds):
                h = Harness(seconds=seconds).run()
                self.assertEqual(h.status, 1, h.output)
                self.assertIn("--seconds must be finite and positive", h.output)
                self.assertEqual(h.calls, [])
                self.assertEqual(h.device.mock_calls, [])
                self.assertIsNone(h.spool)
                self.assertEqual(h.sleep_count, 0)
                self.assertEqual(h.clock, 0.0)

    def test_direct_helper_finite_durations_use_elapsed_clock(self):
        for seconds, step in (("0.25", 0.1), ("1", 0.1),
                              ("1e20", 5e19)):
            with self.subTest(seconds=seconds):
                h = Harness(seconds=seconds, clock_step=step).run()
                self.assertEqual(h.status, 0, h.output)
                self.assertIn("CONSOLE CAPTURE OK", h.output)
                self.assertIn("2 samples observed",
                              fixtures.LINK.evaluate(h.output))
                self.assertIn("launch", h.calls)
                self.assertIn("close", h.calls)
                self.assertGreaterEqual(h.clock - step, float(seconds))
                self.assertLessEqual(h.sleep_count, 12)

    def rejected(self, harness):
        self.assertEqual(harness.status, 1, harness.output)
        self.assertNotIn("CONSOLE CAPTURE OK", harness.output)
        self.assertIn("ICDI CAPTURE FAILURE:", harness.output)
        with self.assertRaises(ValueError):
            fixtures.LINK.evaluate(harness.output)
        # Saved healthy records cannot repair a lifecycle failure.
        metadata = "\n".join(line for line in harness.output.splitlines()
                             if line.startswith("ICDI CAPTURE FAILURE:"))
        with self.assertRaises(ValueError):
            fixtures.LINK.evaluate(fixtures.framed(
                fixtures.HEALTHY, metadata + "\n"))

    def test_partial_acquisition(self):
        for error in ("construct", "open", "sleep1", "set_bitmode",
                      "set_baudrate", "timeouts", "startup-report",
                      "thread-construct", "start",
                      "start-after", "sleep2", "temp", "launch"):
            with self.subTest(error=error):
                h = Harness([error]).run()
                self.rejected(h)
                self.assertEqual("close" in h.calls,
                                 error not in ("construct", "start"))
                if error in ("start-after", "sleep2", "temp", "launch"):
                    self.assertLess(h.calls.index("stop"),
                                    h.calls.index("join"))
                    self.assertLess(h.calls.index("join"),
                                    h.calls.index("close"))
                self.assertNotIn("wait", h.calls)
                self.assertIn(error + " original", h.output)

    def test_optional_flush_contract(self):
        h = Harness(["purge_buffers"]).run()
        self.assertEqual(h.status, 0)
        self.assertIn("ICDI CAPTURE WARNING: flush-error", h.output)
        with self.assertRaises(ValueError):
            fixtures.LINK.evaluate(h.output)

    def test_capture_status_and_sleep_exceptions(self):
        for error in ("poll", "sleep3", "pread1", "fileno"):
            with self.subTest(error=error):
                h = Harness([error]).run()
                self.rejected(h)
                self.assertIn("join", h.calls)
                self.assertIn("wait", h.calls)
                self.assertIn("spool-close", h.calls)
                self.assertIn("close", h.calls)

    def test_cleanup_errors(self):
        for error in ("stop", "join", "alive", "terminate", "wait", "kill",
                      "reap", "pread2", "spool-close", "close"):
            with self.subTest(error=error):
                errors = [error]
                if error in ("kill", "reap"):
                    errors.append("timeout")
                h = Harness(errors).run()
                self.rejected(h)
                self.assertIn(error + " original", h.output)
                self.assertIn("spool-close", h.calls)
                self.assertIn("wait", h.calls)
                if error in ("join", "alive"):
                    self.assertNotIn("close", h.calls)
                    self.assertEqual(h.state["snapshot"], b"")
                else:
                    self.assertIn("close", h.calls)

    def test_snapshot_failure_is_not_evidence(self):
        h = Harness(["snapshot"]).run()
        self.rejected(h)
        self.assertEqual(h.state["snapshot"], b"")
        self.assertIn("lifecycle-snapshot", h.output)
        self.assertIn("close", h.calls)

    def test_live_reader_retains_device_and_no_snapshot(self):
        h = Harness(live=True).run()
        self.rejected(h)
        self.assertNotIn("close", h.calls)
        self.assertEqual(h.state["snapshot"], b"")
        self.assertIn("lifecycle-reader-live", h.output)
        self.assertIn("wait", h.calls)

    def test_late_chunk_and_post_snapshot_mutation(self):
        h = Harness(late=True, mutate=True, raw=True).run()
        self.assertEqual(h.status, 0, h.output)
        self.assertEqual(h.state["snapshot"], RAW)
        self.assertEqual(h.state["captured"], b"\x03X")
        self.assertIn("raw: " + RAW[:64].hex(" "), h.output)
        self.assertIn(fixtures.sample(1), h.output)
        self.assertIn("2 samples observed", fixtures.LINK.evaluate(h.output))

    def test_controlled_concurrent_final_read_before_snapshot(self):
        h = Harness(concurrent=True, mutate=True).run()
        self.assertEqual(h.status, 0, h.output)
        self.assertEqual(h.state["snapshot"], RAW)
        self.assertEqual(h.state["captured"], b"\x03X")
        self.assertIn("2 samples observed", fixtures.LINK.evaluate(h.output))
        self.assertLess(h.calls.index("join"), h.calls.index("close"))

    def test_timeout_kill_final_reap_order(self):
        h = Harness(["timeout"]).run()
        self.assertEqual(h.status, 0, h.output)
        self.assertLess(h.calls.index("terminate"), h.calls.index("wait"))
        self.assertLess(h.calls.index("wait"), h.calls.index("kill"))
        self.assertLess(h.calls.index("kill"), h.calls.index("reap"))
        self.assertLess(h.calls.index("reap"), h.calls.index("spool-close"))

    def test_actual_reader_error_concurrent_launch_failure(self):
        h = Harness(["launch", "close"], actual_reader=True).run()
        self.rejected(h)
        self.assertIn("reader original", h.output)
        self.assertIn("ICDI CAPTURE WARNING: read-error", h.output)
        self.assertEqual(h.state["probe_failure"], "probe-launch-error")
        self.assertIn("lifecycle-device-close", h.output)

    def test_primary_survives_concurrent_cleanup_errors(self):
        h = Harness(["sleep3", "stop", "terminate", "wait", "kill",
                     "reap", "pread1", "spool-close", "close"]).run()
        self.rejected(h)
        self.assertEqual(h.state["probe_failure"], "probe-inspection-error")
        for name in ("sleep3", "stop", "terminate", "wait", "kill", "reap",
                     "pread1", "spool-close", "close"):
            self.assertIn(name + " original", h.output)

    def test_decode_header_report_failures_after_cleanup(self):
        for error in ("decode", "header", "report"):
            with self.subTest(error=error):
                h = Harness([error]).run()
                self.rejected(h)
                self.assertIn("close", h.calls)
                self.assertIn("spool-close", h.calls)
                self.assertIn(error + " original", h.output)

    def test_join_error_after_actual_reader_exit_still_denies_acceptance(self):
        h = Harness(["join"], actual_reader=True).run()
        self.rejected(h)
        self.assertIn("close", h.calls)
        self.assertEqual(h.state["snapshot"], RAW)

    def test_delayed_native_start_with_failed_stop_retains_ownership(self):
        for exception_type in (KeyboardInterrupt, SystemExit):
            with self.subTest(exception_type=exception_type):
                h = Harness(["stop"], delayed_bootstrap=True,
                            exception_type=exception_type).run()
                self.rejected(h)
                self.assertIn("start original", h.output)
                self.assertEqual(h.state["probe_failure"],
                                 "lifecycle-thread-start")
                self.assertNotIn(
                    "ICDI CAPTURE FAILURE: lifecycle-reader-live", h.output)
                self.assertIn("stop original", h.output)
                self.assertIn("cannot join thread before it is started",
                              h.output)
                self.assertFalse(h.state["reader_stopped"])
                self.assertFalse(h.unpublished_status)
                self.assertFalse(h.completion_at_status)
                self.assertNotIn("snapshot", h.calls)
                self.assertNotIn("close", h.calls)
                self.assertEqual(h.state["snapshot"], b"")
                self.assertEqual(h.device.read_data.call_count, 0)
                self.assertIn("alive", h.calls)

    def test_baseexception_reader_and_cleanup_paths(self):
        for exception_type in (KeyboardInterrupt, SystemExit):
            with self.subTest(exception_type=exception_type):
                h = Harness(["launch", "stop", "join"], actual_reader=True,
                            exception_type=exception_type).run()
                self.rejected(h)
                self.assertEqual(h.state["snapshot"], RAW)
                self.assertIn("close", h.calls)
                self.assertTrue(h.state["reader_completed"].is_set())
                self.assertIn("reader original", h.output)
                for name in ("launch", "stop", "join"):
                    self.assertIn(name + " original", h.output)

    def test_lifecycle_grammar_remains_narrow(self):
        for code in ("lifecycle-unknown", "lifecycle-report extra",
                     "lifecycle-device-close=0"):
            with self.subTest(code=code):
                with self.assertRaisesRegex(ValueError, "invalid capture"):
                    fixtures.LINK.evaluate(fixtures.framed(
                        fixtures.HEALTHY,
                        "ICDI CAPTURE FAILURE: " + code + "\n"))


if __name__ == "__main__":
    unittest.main()
