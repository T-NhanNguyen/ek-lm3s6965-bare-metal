"""Offline fixtures and process-seam tests. Never launch the capture helper."""

from contextlib import redirect_stdout
import importlib.util
import io
import itertools
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import Mock, patch


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "ethernet_link_test.py"
)
SPEC = importlib.util.spec_from_file_location("ethernet_link", MODULE_PATH)
LINK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LINK)

HEADER = LINK.IDENTITY + "\nethernet init=OK\n"


def sample(index, healthy=True, speed="100", duplex="full"):
    if healthy:
        state = ("link=up negotiation=complete speed_mbps=%s duplex=%s "
                 "mac_ready=1" % (speed, duplex))
    else:
        state = ("link=down negotiation=pending speed_mbps=unknown "
                 "duplex=unknown mac_ready=0")
    return ("ethernet sample=%d init=OK poll=OK poll_errors=0 %s\n"
            % (index, state))


HEALTHY = HEADER + sample(0) + sample(1)


def framed(text, warning=""):
    return (warning + LINK.BEGIN + "\n" + text + LINK.END + "\n"
            "CONSOLE CAPTURE OK: startup marker 'bring-up complete' received "
            "over SWO through the ICDI\nPeripheral operation was not evaluated.\n")


def mock_helper_capture(text, read_error=False, *, probe_statuses=None,
                        diagnostics="ICDI PROBE READY: init-reset-run\n",
                        teardown_status=-15, launch_error=None,
                        flush_error=False, process=None):
    """Execute the actual helper Python, with every hardware/time seam mocked."""
    source = MODULE_PATH.with_name("icdi-console.sh").read_text()
    source = source.split("<<'PYTHON'\n", 1)[1].rsplit("\nPYTHON", 1)[0]
    ftdi = types.ModuleType("pyftdi.ftdi")
    ftdi.Ftdi = Mock()
    ftdi.Ftdi.BitMode.RESET = 0
    raw = b"".join(b"\x03" + bytes([byte]) for byte in text.encode())

    class ReaderThread:
        def __init__(self, target, **kwargs):
            self.state = target.__globals__

        def start(self):
            self.state["captured"].extend(raw)
            if read_error:
                self.state["read_errors"].append("mock USB read error")

        def join(self, **kwargs):
            self.state["reader_completed"].set()

        def is_alive(self):
            return False

    if flush_error:
        ftdi.Ftdi.return_value.purge_buffers.side_effect = OSError("mock flush")
    process = process or Mock(returncode=None)
    statuses = iter(probe_statuses or [])
    observed_status = None

    def poll():
        nonlocal observed_status
        observed_status = next(statuses, observed_status)
        if observed_status is not None:
            process.returncode = observed_status
        return process.returncode

    def wait(**kwargs):
        if process.returncode is None:
            process.returncode = teardown_status
        return process.returncode

    process.poll.side_effect = poll
    process.wait.side_effect = wait

    def launch(*args, **kwargs):
        if launch_error:
            raise launch_error
        # Exercise the actual spool seam, not a copied diagnostics parser.
        assert kwargs["stderr"] != subprocess.PIPE
        kwargs["stderr"].write(diagnostics.encode())
        kwargs["stderr"].flush()
        command = args[0]
        assert command[:4] == ["openocd", "-f", "mock.cfg", "-c"]
        assert "init; reset run;" in command[4]
        assert "was_examined" in command[4]
        assert "echo {ICDI PROBE READY: init-reset-run}" in command[4]
        return process

    output = io.StringIO()
    with patch.dict("sys.modules", {"pyftdi": types.ModuleType("pyftdi"),
                                    "pyftdi.ftdi": ftdi}), \
            patch("sys.argv", ["-", "mock.cfg", "1000000", "1", "false",
                               "bring-up complete"]), \
            patch("threading.Thread", ReaderThread), \
            patch("time.sleep"), \
            patch("time.monotonic", side_effect=(
                tick / 10 for tick in itertools.count())), \
            patch("subprocess.Popen", side_effect=launch) as probe, \
            redirect_stdout(output):
        with unittest.TestCase().assertRaises(SystemExit) as result:
            exec(compile(source, "icdi-console.sh:embedded-python", "exec"), {})
        probe.assert_called_once()
    return result.exception.code, output.getvalue()


class EvaluationTests(unittest.TestCase):
    def test_healthy_modes(self):
        for speed in ("10", "100"):
            for duplex in ("half", "full"):
                with self.subTest(speed=speed, duplex=duplex):
                    text = (HEADER + sample(0, speed=speed, duplex=duplex)
                            + sample(1, speed=speed, duplex=duplex))
                    summary = LINK.evaluate(text)
                    self.assertIn("%s Mbps %s duplex" % (speed, duplex),
                                  summary)
                    self.assertIn("2 samples observed", summary)

    def test_historical_helper_envelope(self):
        capture = ("Listening on the ICDI at 1000000 8N1 ...\n"
                   "Captured 100 bytes, decoded 20 characters from ITM stimulus port 0.\n"
                   + LINK.SEPARATOR + "\n" + HEALTHY
                   + LINK.SEPARATOR + "\n"
                   "PASS: console received over SWO through the ICDI\n")
        self.assertIn("2 samples observed", LINK.evaluate(capture))
        with self.assertRaisesRegex(ValueError, "transport evidence problem"):
            LINK.evaluate("warning: 1 read errors; first was: mock\n" + capture)

    def test_expected_startup_lines(self):
        text = (LINK.IDENTITY + "\n"
                "clock pll=locked cpu_hz=50000000 rcc=0x01CE1380\n"
                "console uart_baud=115200 swo_baud=1000000\n"
                "ethernet mac=02:00:00:69:65:01 identity=link-test-only tx=none\n"
                "ethernet init=OK\n"
                "Ethernet diagnostic bring-up complete. Link acceptance pending\n"
                + sample(0) + sample(1))
        self.assertIn("2 samples observed", LINK.evaluate(framed(text)))
        for altered in (text.replace("cpu_hz=50000000", "cpu_hz=123"),
                        text.replace("uart_baud=115200", "uart_baud=1"),
                        text.replace("identity=link-test-only", "identity=other"),
                        text + "CONSOLE CAPTURE OK: startup marker 'bring-up complete' "
                        "received over SWO through the ICDI\n"):
            with self.subTest(text=altered):
                with self.assertRaises(ValueError):
                    LINK.evaluate(framed(altered))

    def test_startup_down_then_up(self):
        summary = LINK.evaluate(
            HEADER + sample(0, False) + sample(1) + sample(2)
        )
        self.assertIn("3 samples observed", summary)
        self.assertIn("2 consecutive final healthy samples", summary)

    def test_rejections(self):
        fixtures = {
            "all down": HEADER + sample(0, False) + sample(1, False),
            "final down": HEALTHY + sample(2, False),
            "single up": HEADER + sample(0, False) + sample(1),
            "nonconsecutive up": HEALTHY + sample(2, False) + sample(3),
            "final pending": HEALTHY + sample(2, False).replace(
                "link=down", "link=up"),
            "no samples": HEADER,
            "missing identity": HEALTHY.replace(LINK.IDENTITY, ""),
            "wrong application": HEALTHY.replace(
                LINK.IDENTITY, "LM3S6965 baremetal diagnostic"),
            "identity suffix": HEALTHY.replace(
                LINK.IDENTITY, LINK.IDENTITY + " extra"),
            "missing init": HEALTHY.replace("ethernet init=OK\n", ""),
            "init failure": HEALTHY.replace(
                "ethernet init=OK", "ethernet init=TIMEOUT"),
            "sample init failure": HEALTHY.replace(
                "sample=0 init=OK", "sample=0 init=TIMEOUT"),
            "poll error": HEALTHY.replace("poll=OK", "poll=TIMEOUT", 1),
            "cumulative error": HEALTHY.replace(
                "poll_errors=0", "poll_errors=1", 1),
            "unknown speed": HEALTHY.replace("speed_mbps=100",
                                               "speed_mbps=unknown"),
            "invalid speed": HEALTHY.replace("speed_mbps=100",
                                               "speed_mbps=1000"),
            "unknown duplex": HEALTHY.replace("duplex=full",
                                                "duplex=unknown"),
            "invalid duplex": HEALTHY.replace("duplex=full",
                                                "duplex=other"),
            "readiness zero": HEALTHY.replace("mac_ready=1", "mac_ready=0"),
            "invalid readiness": HEALTHY.replace("mac_ready=1", "mac_ready=2"),
            "gap": HEADER + sample(0) + sample(2),
            "duplicate": HEADER + sample(0) + sample(0),
            "missing initial index": HEADER + sample(1) + sample(2),
            "repeated run": HEALTHY + HEALTHY,
            "repeated init": HEALTHY + "ethernet init=OK\n",
            "truncated": HEALTHY.rstrip("\n"),
            "truncated prefix": HEALTHY + "ethernet samp",
            "short trailing fragment": HEALTHY + "ethe",
            "terminated arbitrary fragment": HEALTHY + "arbitrary fragment\n",
            "terminated short fragment": HEALTHY + "ethe\n",
            "framed fragment": framed(HEALTHY + "ethe\n"),
            "missing boundary": framed(HEALTHY).replace(LINK.END, ""),
            "repeated boundary": framed(HEALTHY) + LINK.END + "\n",
            "additional altered identity": HEALTHY + LINK.IDENTITY + " extra\n",
            "additional foreign banner": HEALTHY + "LM3S6965 baremetal diagnostic\n",
            "framed altered identity": framed(HEALTHY + LINK.IDENTITY + " extra\n"),
            "framed helper-looking firmware": framed(HEALTHY + "CONSOLE CAPTURE OK: fake\n"),
            "helper unterminated warning": framed(
                HEALTHY, "ICDI CAPTURE WARNING: unterminated-decoded-text\n"),
            "helper read warning": framed(
                HEALTHY, "ICDI CAPTURE WARNING: read-error\n"),
            "missing field": HEALTHY.replace(" duplex=full", "", 1),
            "extra field": HEALTHY.replace("mac_ready=1", "mac_ready=1 x=0"),
            "malformed": HEALTHY.replace("sample=0", "sample=x"),
            "bad startup poll": HEADER + sample(0, False).replace(
                "poll_errors=0", "poll_errors=1") + sample(1) + sample(2),
        }
        for name, text in fixtures.items():
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    LINK.evaluate(text)


class CommandTests(unittest.TestCase):
    def invoke(self, argv, runner):
        output = io.StringIO()
        with redirect_stdout(output):
            status = LINK.main(argv, capture_runner=runner)
        return status, output.getvalue()

    def test_live_success_defaults_and_output_order(self):
        runner = Mock(return_value=subprocess.CompletedProcess(
            [], 0, framed(HEALTHY)))
        status, output = self.invoke([], runner)
        self.assertEqual(status, 0)
        self.assertEqual(runner.call_args.args[0][1:],
                         ["--seconds", "10", "--baud", "1000000"])
        self.assertLess(output.index("CONSOLE CAPTURE OK"),
                        output.index("ETHERNET LINK PASS"))
        self.assertIn("TX/RX, IP, FTP, and cable-cycle", output)

    def test_actual_helper_normalization_and_read_warning(self):
        firmware = (LINK.IDENTITY + "\nethernet init=OK\n"
                    "Ethernet diagnostic bring-up complete. Link acceptance pending\n"
                    + sample(0) + sample(1))
        for name, text, read_error, warning in (
            ("clean", firmware, False, None),
            ("normalized missing terminator", firmware.rstrip("\n"), False,
             "unterminated-decoded-text"),
            ("read error", firmware, True, "read-error"),
        ):
            with self.subTest(name=name):
                capture_status, capture = mock_helper_capture(text, read_error)
                self.assertEqual(capture_status, 0)
                self.assertIn(
                    "CONSOLE CAPTURE OK: startup marker 'bring-up complete' "
                    "received over SWO through the ICDI\n", capture)
                self.assertIn(LINK.BEGIN + "\n", capture)
                # This is the helper-normalized form, not direct truncation.
                self.assertIn(sample(1) + LINK.END + "\n", capture)
                runner = Mock(return_value=subprocess.CompletedProcess(
                    [], capture_status, capture))
                status, output = self.invoke([], runner)
                self.assertEqual(status, 1 if warning else 0)
                if warning:
                    self.assertIn("ICDI CAPTURE WARNING: " + warning, capture)
                    self.assertIn("capture transport evidence problem", output)
                    self.assertNotIn("ETHERNET LINK PASS", output)
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory) / "capture.log"
                        path.write_text(capture)
                        offline = Mock(side_effect=AssertionError("no hardware"))
                        status, output = self.invoke(
                            ["--capture-file", str(path)], offline)
                    self.assertEqual(status, 1)
                    offline.assert_not_called()
                    self.assertIn("capture transport evidence problem", output)

    def test_actual_helper_probe_failures(self):
        firmware = (HEADER
                    + "Ethernet diagnostic bring-up complete. "
                    "Link acceptance pending\n" + sample(0) + sample(1))
        cases = (
            ("startup exit", [1], "Error : probe setup failed\n",
             "probe-exit status=1"),
            ("reset error despite receipt", [],
             "Error : reset failed\nICDI PROBE READY: init-reset-run\n",
             "probe-command-error"),
            ("setup error", [], "Error : init failed\n",
             "probe-command-error"),
            ("unconfirmed setup", [], "Info : listening\n",
             "probe-command-unconfirmed"),
            ("duplicate receipt", [],
             "ICDI PROBE READY: init-reset-run\n" * 2,
             "probe-command-unconfirmed"),
            ("midway zero", [None, None, 0], "Info : useful detail\n",
             "probe-exit status=0"),
            ("midway nonzero", [None, None, 2], "Error : disconnected\n",
             "probe-exit status=2"),
            ("pre-teardown exit", [None] * 11 + [0],
             "ICDI PROBE READY: init-reset-run\n", "probe-exit status=0"),
            ("oversized diagnostics", [], "x" * 65537,
             "probe-diagnostics-overflow"),
        )
        for name, statuses, diagnostics, failure in cases:
            with self.subTest(name=name):
                process = Mock(returncode=None)
                capture_status, capture = mock_helper_capture(
                    firmware, probe_statuses=statuses,
                    diagnostics=diagnostics, process=process)
                self.assertEqual(capture_status, 1)
                self.assertIn("ICDI CAPTURE FAILURE: " + failure, capture)
                self.assertNotIn("CONSOLE CAPTURE OK", capture)
                self.assertIn(sample(1), capture)
                self.assertIn("ICDI PROBE STDERR: ", capture)
                if "probe-exit" in failure:
                    process.terminate.assert_not_called()
                runner = Mock(return_value=subprocess.CompletedProcess(
                    [], capture_status, capture))
                status, output = self.invoke([], runner)
                self.assertEqual(status, 1)
                self.assertNotIn("ETHERNET LINK PASS", output)
                # A zero runner result cannot override retained failure text.
                runner.return_value.returncode = 0
                status, output = self.invoke([], runner)
                self.assertEqual(status, 1)
                self.assertNotIn("ETHERNET LINK PASS", output)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "capture.txt"
                    path.write_text(capture)
                    offline = Mock(side_effect=AssertionError("no hardware"))
                    status, output = self.invoke(
                        ["--capture-file", str(path)], offline)
                offline.assert_not_called()
                self.assertEqual(status, 1)
                self.assertNotIn("ETHERNET LINK PASS", output)

    def test_actual_helper_exit_discovered_inside_terminate(self):
        firmware = (HEADER
                    + "Ethernet diagnostic bring-up complete. "
                    "Link acceptance pending\n" + sample(0) + sample(1))
        for exit_status in (0, 1):
            with self.subTest(exit_status=exit_status):
                process = Mock(returncode=None)
                signal_sent = Mock()

                def terminate():
                    # Model Popen's internal poll discovering a natural exit.
                    # No signal is sent, despite the helper's last live poll.
                    process.returncode = exit_status
                    if process.returncode is not None:
                        return
                    signal_sent()

                process.terminate.side_effect = terminate
                capture_status, capture = mock_helper_capture(
                    firmware, process=process, teardown_status=exit_status)
                process.terminate.assert_called_once()
                process.wait.assert_called_once_with(timeout=5)
                signal_sent.assert_not_called()
                self.assertEqual(capture_status, 1)
                self.assertIn("ICDI CAPTURE FAILURE: probe-exit status=%d"
                              % exit_status, capture)
                self.assertNotIn("ICDI PROBE STATUS:", capture)
                self.assertNotIn("CONSOLE CAPTURE OK", capture)
                self.assertIn(sample(1), capture)
                for runner_status in (capture_status, 0):
                    runner = Mock(return_value=subprocess.CompletedProcess(
                        [], runner_status, capture))
                    status, output = self.invoke([], runner)
                    self.assertEqual(status, 1)
                    self.assertNotIn("ETHERNET LINK PASS", output)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "capture.txt"
                    path.write_text(capture)
                    offline = Mock(side_effect=AssertionError("no hardware"))
                    status, output = self.invoke(
                        ["--capture-file", str(path)], offline)
                offline.assert_not_called()
                self.assertEqual(status, 1)
                self.assertNotIn("ETHERNET LINK PASS", output)

    def test_actual_helper_healthy_probe_and_harmless_stderr(self):
        firmware = (HEADER
                    + "Ethernet diagnostic bring-up complete. "
                    "Link acceptance pending\n" + sample(0) + sample(1))
        for teardown_status in (0, -15, 1):
            with self.subTest(teardown_status=teardown_status):
                process = Mock(returncode=None)
                signal_sent = Mock()

                def terminate():
                    self.assertIsNone(process.returncode)
                    signal_sent()
                    # Sending a signal does not itself reap the child.

                process.terminate.side_effect = terminate
                diagnostics = ("Info : harmless setup detail\n"
                               "Warn : harmless warning\n"
                               "ethernet sample=not-firmware\n"
                               "ICDI PROBE READY: init-reset-run\n")
                status, capture = mock_helper_capture(
                    firmware, diagnostics=diagnostics, process=process,
                    teardown_status=teardown_status)
                self.assertEqual(status, 0)
                process.terminate.assert_called_once()
                signal_sent.assert_called_once_with()
                process.wait.assert_called_once_with(timeout=5)
                self.assertEqual(process.returncode, teardown_status)
                self.assertIn('ICDI PROBE STDERR: "Warn : harmless warning"',
                              capture)
                self.assertIn('ICDI PROBE STDERR: '
                              '"ethernet sample=not-firmware"', capture)
                self.assertNotIn("ICDI CAPTURE FAILURE:", capture)
                self.assertIn("2 samples observed", LINK.evaluate(capture))

    def test_actual_helper_status_inspection_failure(self):
        process = Mock(returncode=None)
        class BrokenStatuses:
            failed = False

            def __iter__(self):
                return self

            def __next__(self):
                if self.failed:
                    raise StopIteration
                self.failed = True
                raise OSError("mock status failed")

        status, capture = mock_helper_capture(
            HEALTHY, probe_statuses=BrokenStatuses(), process=process)
        self.assertEqual(status, 1)
        self.assertIn("ICDI CAPTURE FAILURE: probe-inspection-error", capture)
        self.assertIn("mock status failed", capture)
        self.assertNotIn("CONSOLE CAPTURE OK", capture)

    def test_actual_helper_launch_failure(self):
        status, capture = mock_helper_capture(
            HEALTHY, launch_error=OSError("mock probe launch failed"))
        self.assertEqual(status, 1)
        self.assertIn("ICDI CAPTURE FAILURE: probe-launch-error", capture)
        self.assertIn("mock probe launch failed", capture)
        self.assertNotIn("CONSOLE CAPTURE OK", capture)

    def test_actual_helper_flush_warning(self):
        firmware = (HEADER
                    + "Ethernet diagnostic bring-up complete. "
                    "Link acceptance pending\n" + sample(0) + sample(1))
        status, capture = mock_helper_capture(firmware, flush_error=True)
        self.assertEqual(status, 0)
        self.assertIn("ICDI CAPTURE WARNING: flush-error", capture)
        with self.assertRaisesRegex(ValueError, "transport evidence problem"):
            LINK.evaluate(capture)

    def test_strict_probe_metadata(self):
        for line in (
            "ICDI CAPTURE FAILURE: arbitrary-error",
            "ICDI CAPTURE FAILURE: probe-exit status=zero",
            "ICDI CAPTURE FAILURE: probe-exit status=0 extra",
            "ICDI PROBE STDERR: unquoted text",
            "ICDI PROBE STDERR: {}",
            "ICDI PROBE STDERR: 0",
            'ICDI PROBE STDERR: "ok" trailing',
            "ICDI PROBE STATUS: unknown",
            "CONSOLE CAPTURE FAIL: arbitrary failure",
        ):
            with self.subTest(line=line):
                with self.assertRaises(ValueError):
                    LINK.evaluate(framed(HEALTHY, line + "\n"))
        with self.assertRaises(ValueError):
            LINK.evaluate(framed(HEALTHY + 'ICDI PROBE STDERR: "info"\n'))
        with self.assertRaises(ValueError):
            LINK.evaluate(framed(HEALTHY, 'ICDI PROBE STDERR: "info"\n')
                          .replace(LINK.END, ""))

    def test_negative_capture_status_conversion(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], -15,
                                                                HEALTHY))
        status, output = self.invoke([], runner)
        self.assertEqual(status, 143)
        self.assertIn("console capture exit status -15", output)
        self.assertNotIn("ETHERNET LINK PASS", output)

    def test_live_options(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 0, HEALTHY))
        status, _ = self.invoke(["--seconds", "12", "--baud", "2000000"],
                                runner)
        self.assertEqual(status, 0)
        self.assertEqual(runner.call_args.args[0][1:],
                         ["--seconds", "12", "--baud", "2000000"])

    def test_capture_failure_cannot_pass_with_healthy_text(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 7, HEALTHY))
        status, output = self.invoke([], runner)
        self.assertEqual(status, 7)
        self.assertIn(HEALTHY, output)
        self.assertIn("ETHERNET LINK FAIL", output)
        self.assertNotIn("ETHERNET LINK PASS", output)

    def test_capture_launch_failure(self):
        runner = Mock(side_effect=OSError("capture unavailable"))
        status, output = self.invoke([], runner)
        self.assertEqual(status, 1)
        self.assertIn("ETHERNET LINK FAIL", output)

    def test_live_invalid_text(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 0, HEADER))
        status, output = self.invoke([], runner)
        self.assertEqual(status, 1)
        self.assertIn("ETHERNET LINK FAIL", output)

    def test_offline_text_only(self):
        runner = Mock(side_effect=AssertionError("hardware must not run"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "decoded.txt"
            path.write_text(HEALTHY, encoding="utf-8")
            status, output = self.invoke(["--capture-file", str(path)], runner)
        runner.assert_not_called()
        self.assertEqual(status, 0)
        self.assertIn("OFFLINE: text-only", output)
        self.assertIn("no live capture provenance", output)
        self.assertIn("Capture exit status is unknown", output)

    def test_offline_shell_entry_point(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "decoded.txt"
            path.write_text(HEALTHY, encoding="utf-8")
            result = subprocess.run(
                [str(MODULE_PATH.with_name("ethernet-link-test.sh")),
                 "--capture-file", str(path)],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ETHERNET LINK PASS", result.stdout)
        self.assertIn("OFFLINE: text-only", result.stdout)

    def test_missing_file(self):
        runner = Mock(side_effect=AssertionError("hardware must not run"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "absent.txt"
            status, output = self.invoke(["--capture-file", str(path)], runner)
        runner.assert_not_called()
        self.assertEqual(status, 1)
        self.assertIn("ETHERNET LINK FAIL", output)

    def test_help_does_not_capture(self):
        runner = Mock(side_effect=AssertionError("hardware must not run"))
        with redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(SystemExit) as error:
                LINK.main(["--help"], capture_runner=runner)
        self.assertEqual(error.exception.code, 0)
        runner.assert_not_called()
        self.assertIn("Offline mode", output.getvalue())


if __name__ == "__main__":
    unittest.main()
