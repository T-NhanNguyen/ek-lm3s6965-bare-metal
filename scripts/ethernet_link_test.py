"""Evaluate one Ethernet link diagnostic run using only the standard library."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


IDENTITY = "LM3S6965 Ethernet link diagnostic"
SAMPLE = re.compile(
    r"ethernet sample=([0-9]+) init=OK poll=OK poll_errors=0 "
    r"link=(up|down) negotiation=(complete|pending) "
    r"speed_mbps=(unknown|10|100) duplex=(unknown|half|full) "
    r"mac_ready=([01])"
)
LIMITS = (
    "Link only. TX/RX, IP, FTP, and cable-cycle operation were not evaluated."
)


SEPARATOR = "-" * 62
BEGIN = "ICDI DECODED TEXT BEGIN"
END = "ICDI DECODED TEXT END"
PROBE_STDERR = "ICDI PROBE STDERR: "
CAPTURE_FAILURE = re.compile(
    r"ICDI CAPTURE FAILURE: (?:probe-exit status=-?[0-9]+|"
    r"probe-command-error|probe-command-unconfirmed|probe-launch-error|"
    r"probe-inspection-error|probe-diagnostics-overflow|"
    r"lifecycle-(?:device-construct|device-open|setup-sleep|device-flush|"
    r"device-config|thread-start|reader-sleep|diagnostic-open|reader-stop|"
    r"reader-status|reader-join|reader-live|snapshot|probe-status|"
    r"probe-terminate|probe-wait|probe-kill|probe-reap|diagnostic-read|"
    r"diagnostic-close|device-close|report))"
)
HELPER_LINES = (
    re.compile(r"Listening on the ICDI at [0-9]+ 8N1 \.\.\."),
    re.compile(r"Captured [0-9]+ bytes, decoded [0-9]+ characters from ITM stimulus port 0\."),
    re.compile(r"raw: [0-9a-f ]*"),
    re.compile(r"CONSOLE CAPTURE OK: startup marker 'bring-up complete' received over SWO through the ICDI"),
)
HELPER_FIXED = {
    "", SEPARATOR,
    "Resetting the target -- the banner is printed once, right now.",
    "The probe stays attached for the whole capture window, because the board's",
    "CPLD routes SWO only while the probe asserts its SWD_EN line.",
    "Peripheral operation was not evaluated.",
    "ICDI PROBE STATUS: init-reset-run completed, alive before teardown",
    "PASS: console received over SWO through the ICDI",
}
STARTUP = (
    re.compile(r"clock pll=(?:locked cpu_hz=50000000|TIMEOUT_xtal_fallback cpu_hz=8000000) rcc=0x[0-9A-F]{8}"),
    re.compile(r"console uart_baud=115200 swo_baud=1000000"),
    re.compile(r"ethernet mac=02:00:00:69:65:01 identity=link-test-only tx=none"),
    re.compile(r"Ethernet diagnostic bring-up complete\. Link acceptance pending"),
)


def decoded_payload(text):
    """Accept bare firmware, framed helper output, or historical two-rule output.

    Legacy saved captures lack termination metadata: they cannot prove that the
    old helper did not normalize a missing newline. New captures must retain
    both boundaries and all warning lines when saved for offline evaluation.
    """
    lines = text.splitlines(keepends=True)
    stripped = [line.rstrip("\r\n") for line in lines]
    for line in stripped:
        if line.startswith("ICDI CAPTURE FAILURE:"):
            if not CAPTURE_FAILURE.fullmatch(line):
                raise ValueError("invalid capture failure metadata")
            raise ValueError("capture transport evidence problem: " + line)
        if line.startswith(("ICDI CAPTURE WARNING:", "warning:")):
            raise ValueError("capture transport evidence problem: " + line)
    if BEGIN in stripped or END in stripped:
        if stripped.count(BEGIN) != 1 or stripped.count(END) != 1:
            raise ValueError("capture transport evidence problem: invalid decoded boundaries")
        start, end = stripped.index(BEGIN), stripped.index(END)
        if start >= end:
            raise ValueError("capture transport evidence problem: reversed decoded boundaries")
    elif SEPARATOR in stripped:
        if stripped.count(SEPARATOR) != 2 or not any(
            HELPER_LINES[1].fullmatch(line) for line in stripped
        ):
            raise ValueError("capture transport evidence problem: invalid historical envelope")
        start = stripped.index(SEPARATOR)
        end = stripped.index(SEPARATOR, start + 1)
    else:
        return text
    for line in stripped[:start] + stripped[end + 1:]:
        if line.startswith(PROBE_STDERR):
            # JSON strings keep child diagnostics distinct from firmware.
            try:
                diagnostic = json.loads(line[len(PROBE_STDERR):])
            except ValueError as error:
                raise ValueError("invalid probe stderr metadata") from error
            if not isinstance(diagnostic, str):
                raise ValueError("invalid probe stderr metadata")
            continue
        if line not in HELPER_FIXED and not any(
            pattern.fullmatch(line) for pattern in HELPER_LINES
        ):
            raise ValueError("capture transport evidence problem: unrecognized helper envelope")
    return "".join(lines[start + 1:end])


def evaluate(text):
    """Return a summary or reject malformed, unhealthy, or mixed-run text."""
    text = decoded_payload(text)
    startup_seen = set()
    identity_seen = False
    init_seen = False
    samples = []
    final_healthy = []
    for raw_line in text.splitlines(keepends=True):
        line = raw_line.rstrip("\r\n")
        if line and not raw_line.endswith("\n"):
            raise ValueError("unterminated decoded firmware text")
        startup = next((index for index, pattern in enumerate(STARTUP)
                        if pattern.fullmatch(line)), None)
        if startup is not None:
            if not identity_seen or samples or startup in startup_seen:
                raise ValueError("repeated or misplaced startup record")
            startup_seen.add(startup)
        elif line == IDENTITY:
            if identity_seen or init_seen or samples:
                raise ValueError("repeated or misplaced diagnostic identity")
            identity_seen = True
        elif line.startswith("ethernet init"):
            if not identity_seen or init_seen or samples:
                raise ValueError("missing identity or repeated initialization")
            if line != "ethernet init=OK":
                raise ValueError("initialization is not OK")
            init_seen = True
        elif line.startswith("ethernet sample") or "sample=" in line:
            if not identity_seen or not init_seen:
                raise ValueError("sample precedes identity or init=OK")
            match = SAMPLE.fullmatch(line)
            if match is None or not raw_line.endswith("\n"):
                raise ValueError("invalid or truncated Ethernet sample record")
            index, link, negotiation, speed, duplex, ready = match.groups()
            if int(index) != len(samples):
                raise ValueError("sample indices must start at 0 without gaps")
            negotiated = link == "up" and negotiation == "complete"
            if not negotiated and (speed != "unknown" or duplex != "unknown"):
                raise ValueError("unnegotiated sample has speed or duplex")
            if negotiated and (speed == "unknown" or duplex == "unknown"):
                raise ValueError("negotiated sample lacks speed or duplex")
            healthy = negotiated and ready == "1"
            samples.append(match.groups())
            if healthy:
                final_healthy.append((speed, duplex))
            else:
                final_healthy.clear()
        elif line:
            raise ValueError("unrecognized or truncated diagnostic firmware text")
    if not identity_seen:
        raise ValueError("exact Ethernet diagnostic identity was not received")
    if not init_seen:
        raise ValueError("ethernet init=OK was not received")
    if len(final_healthy) < 2:
        raise ValueError("need at least two consecutive final healthy samples")
    modes = list(dict.fromkeys(final_healthy))
    negotiated_modes = ", ".join(
        "%s Mbps %s duplex" % mode for mode in modes
    )
    return (
        "%s, %d samples observed, %d consecutive final healthy samples"
        % (negotiated_modes, len(samples), len(final_healthy))
    )


def positive_seconds(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("seconds must be finite and positive")
    return value


def positive_baud(value):
    if not value.isdecimal() or int(value) <= 0:
        raise argparse.ArgumentTypeError("baud must be a positive integer")
    return value


def main(argv=None, capture_runner=None):
    parser = argparse.ArgumentParser(
        prog="scripts/ethernet-link-test.sh",
        description="Evaluate Ethernet PHY link diagnostic samples.",
        epilog=(
            "Live mode resets the target through icdi-console.sh and requires "
            "a successful capture exit before evaluation. Offline mode reads "
            "saved decoded text only, with no reset or device access. It "
            "cannot establish capture exit status or live provenance. "
            "Accepts bare firmware text, ICDI DECODED TEXT BEGIN/END framed "
            "captures (retain all ICDI CAPTURE WARNING, FAILURE, and probe "
            "diagnostic lines), and historical "
            "two-separator helper captures. Historical captures cannot prove "
            "that the old helper did not normalize a missing newline. "
            + LIMITS + " Sample indices do not measure wall-clock duration."
        ),
    )
    parser.add_argument("--seconds", type=positive_seconds, default=None,
                        help="live capture window in seconds (default: 10)")
    parser.add_argument("--baud", type=positive_baud, default=None,
                        help="live SWO baud rate (default: 1000000)")
    parser.add_argument("--capture-file", type=Path, metavar="PATH",
                        help="offline text-only evaluation of decoded capture")
    args = parser.parse_args(argv)
    if args.capture_file is not None and (
        args.seconds is not None or args.baud is not None
    ):
        parser.error("--seconds and --baud apply only to live capture")
    try:
        if args.capture_file is not None:
            print("OFFLINE: text-only evaluation, no live capture provenance.")
            print("Capture exit status is unknown and is not evaluated.")
            text = args.capture_file.read_text(encoding="utf-8")
        else:
            helper = Path(__file__).with_name("icdi-console.sh")
            runner = capture_runner or subprocess.run
            result = runner(
                [str(helper), "--seconds", args.seconds or "10",
                 "--baud", args.baud or "1000000"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, errors="replace", check=False,
            )
            text = result.stdout
            print(text, end="" if text.endswith("\n") else "\n")
            if result.returncode != 0:
                print("ETHERNET LINK FAIL: console capture exit status %d"
                      % result.returncode)
                print(LIMITS)
                return (result.returncode if result.returncode > 0
                        else 128 - result.returncode)
        if args.capture_file is not None:
            print(text, end="" if text.endswith("\n") else "\n")
        summary = evaluate(text)
    except (OSError, UnicodeError, ValueError) as error:
        print("ETHERNET LINK FAIL: %s" % error)
        print(LIMITS)
        return 1
    print("ETHERNET LINK PASS: %s" % summary)
    print(LIMITS)
    print("Sample indices do not measure wall-clock duration.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
