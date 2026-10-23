"""Two-command restricted-bench demo (not an authenticated network channel).

status works with older firmware. download requires boot-report support, uploads
once, and waits for a physical reset; Ctrl-C never cancels or retries a STOR.
"""
import argparse
import math
import os
from pathlib import Path
import re
import stat
import sys
import time

from .ftp_transport import FTPTransport, ProtocolError
from .workflow import Artifact, Workflow, status

APPLICATIONS = {"A": "slot-a-breathing", "B": "slot-b-blink"}


def load_directory(directory):
    """Validate exactly one top-level package per metadata slot before any I/O."""
    if not directory.is_dir():
        raise ValueError("download requires a package directory")
    packages = {}
    for path in sorted(directory.glob("*.pkg")):
        # A bounded, nonblocking descriptor prevents FIFO/device reads and TOCTOU
        # growth from turning offline validation into an unbounded operation.
        if not stat.S_ISREG(path.stat().st_mode):
            raise ValueError("package must be a regular file")
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or not 396 <= info.st_size <= 111776:
                raise ValueError("package size outside limits")
            artifact = Artifact.from_bytes(stream.read(111777))
        if artifact.slot in packages:
            raise ValueError("duplicate package slot " + artifact.slot)
        packages[artifact.slot] = artifact.body
    if set(packages) != {"A", "B"}:
        raise ValueError("directory needs exactly one valid A and B .pkg package")
    return packages


def boot_report(body):
    """Parse the bounded ASCII application prefix plus unchanged boot-status1.

    The application message is descriptive only. Its attached snapshot must be
    checked against a fresh authoritative status before confirmation.
    """
    if (not isinstance(body, bytes) or len(body) > 2048 or
            any(byte != 10 and not 32 <= byte <= 126 for byte in body)):
        raise ValueError("boot-report must be bounded printable ASCII")
    prefix, separator, snapshot = body.partition(b"\n\n")
    if not separator:
        raise ValueError("boot-report framing; update MCU firmware for report support")
    match = re.fullmatch(
        rb"format=1\napplication=(slot-a-breathing|slot-b-blink)\nmessage=([^\n]+)", prefix)
    if match is None:
        raise ValueError("boot-report prefix")
    application, message = (part.decode("ascii") for part in match.groups())
    lead = application + " booted successfully. "
    if not message.startswith(lead) or not message[len(lead):].strip():
        raise ValueError("boot-report message")
    return application, message, status(snapshot)


def display(label, observed, out):
    print(label, file=out)
    print("".join(f"{key}={value}\n" for key, value in observed.items()), end="", file=out)


IMAGE_KEYS = ("running_slot", "hash", "release")
ROLE_KEYS = ("confirmed_slot", "candidate_slot", "permitted_slot", "trial_state")


def complete_identity(observed):
    if any(observed[key] == "none" for key in IMAGE_KEYS + ("sequence", "trial_id")):
        raise ValueError("incomplete MCU image/trial authority")


def execution_roles(observed):
    """Validate executing roles against the MCU renderer, independent of sequence."""
    slot = observed["running_slot"]
    peer = "B" if slot == "A" else "A"
    if slot not in APPLICATIONS:
        raise ValueError("incoherent MCU execution roles")
    if observed["trial_state"] == "trial":
        expected = (peer, slot, "none", "trial")
    elif observed["trial_state"] == "none":
        # Durable confirmation reopens upload permission for the other slot.
        expected = (slot, "none", peer, "none")
    else:
        raise ValueError("incoherent MCU execution roles")
    if tuple(observed[key] for key in ROLE_KEYS) != expected:
        raise ValueError("incoherent MCU execution roles")


def require_healthy(observed):
    """The demo requires live health even for an already-confirmed image."""
    if observed["health"] != "00000007" or observed["fault"] != "0":
        raise ValueError("MCU health/fault prevents confirmation/success")


def forward_snapshot(previous, current):
    """Compare immutable identity and ordered roles, not changing diagnostics."""
    complete_identity(current)
    execution_roles(previous)
    execution_roles(current)
    if any(previous[key] != current[key] for key in IMAGE_KEYS + ("trial_id",)):
        raise ValueError("MCU image/trial identity changed")
    if int(current["sequence"]) < int(previous["sequence"]):
        raise ValueError("stale MCU sequence")
    if any(previous[key] != current[key] for key in ROLE_KEYS):
        if not (previous["trial_state"] == "trial" and
                current["trial_state"] == "none" and
                int(current["sequence"]) > int(previous["sequence"])):
            raise ValueError("backwards MCU state or changed roles")


def accept_ready(flow, observed):
    """Bind READY to the admitted old image and this acceptance sequence."""
    complete_identity(observed)
    before = flow.before_upload
    complete_identity(before)
    if (any(observed[key] != before[key] for key in IMAGE_KEYS + ("confirmed_slot",)) or
            observed["candidate_slot"] != flow.artifact.slot or
            observed["permitted_slot"] != "none" or
            observed["trial_state"] != "ready" or observed["fault"] != "0" or
            not int(before["sequence"]) < int(observed["trial_id"]) <= int(observed["sequence"]) or
            (flow.trial_id is not None and flow.trial_id != int(observed["trial_id"]))):
        raise ValueError("READY does not bind admitted image and fresh acceptance trial")
    flow.trial_id = int(observed["trial_id"])


def download(flow, packages, *, out, sleep, clock, poll_interval, deadline):
    """Use Workflow admission/classification; never repeat an uncertain upload."""
    initial = flow.read()
    complete_identity(initial)
    display("Initial MCU status (authenticated running image identity):", initial, out)
    flow.upload(packages)  # Admission reads fresh status; MCU selects the slot.
    complete_identity(flow.before_upload)
    if (any(initial[key] != flow.before_upload[key] for key in IMAGE_KEYS + ROLE_KEYS + ("trial_id",)) or
            int(flow.before_upload["sequence"]) < int(initial["sequence"])):
        raise ValueError("upload admission changed from initial MCU identity")
    artifact = flow.artifact
    print(f"Selected package identity (MCU identity match pending): destination={artifact.slot} "
          f"hash={artifact.digest} release={artifact.release}", file=out)
    end = clock() + deadline
    waiting_reset = False
    confirmation_sent = False
    report_checked = False
    previous = None
    ready_sequence = None

    def ready_failure(message):
        # Poll reconnects may fail differently; report the original upload only.
        if flow.upload_diagnostic is not None:
            message += "; upload diagnostic: " + str(flow.upload_diagnostic)
        return ValueError(message)

    while True:
        try:
            observed = flow.read()
        except ProtocolError:
            raise
        except OSError:
            # Reset breaks FTP. Retry reads only, with transport-level deadlines.
            if not waiting_reset and clock() >= end:
                raise ready_failure("status unavailable; upload outcome unknown; do not reupload")
            sleep(poll_interval)
            continue
        if observed["trial_state"] == "ready":
            accept_ready(flow, observed)
            if previous is not None:
                raise ValueError("backwards MCU state to READY")
            if ready_sequence is not None and int(observed["sequence"]) < ready_sequence:
                raise ValueError("stale READY sequence")
            ready_sequence = int(observed["sequence"])
        if waiting_reset or observed["running_slot"] == artifact.slot or observed["trial_state"] == "trial":
            complete_identity(observed)
        if ready_sequence is not None and int(observed["sequence"]) < ready_sequence:
            raise ValueError("stale MCU sequence after READY")
        state = "UNKNOWN" if observed["trial_state"] == "ready" else flow.classify(observed)
        if observed["running_slot"] == artifact.slot and state not in ("TRIAL", "UNHEALTHY", "CONFIRMED"):
            raise ValueError("incoherent MCU execution authority")
        if state in ("TRIAL", "UNHEALTHY", "CONFIRMED"):
            complete_identity(observed)
            execution_roles(observed)
            if state == "CONFIRMED":
                require_healthy(observed)
            if observed["fault"] != "0":
                raise ValueError("MCU fault prevents confirmation/success")
            if ready_sequence is not None and int(observed["sequence"]) <= ready_sequence:
                raise ValueError("stale execution sequence after READY")
            if previous is not None:
                forward_snapshot(previous, observed)
            previous = observed
        elif previous is not None:
            raise ValueError("lost MCU image/trial authority")
        fell_back = (waiting_reset and observed["candidate_slot"] == "none" and
                     observed["running_slot"] == flow.before_upload["running_slot"] and
                     observed["confirmed_slot"] == flow.before_upload["confirmed_slot"])
        if state == "MISMATCH" or observed["trial_state"] == "rejected" or fell_back:
            raise ValueError("image mismatch or fallback; inspect MCU status; no confirmation sent")
        if (observed["candidate_slot"] == artifact.slot and
                observed["trial_state"] == "ready" and not waiting_reset):
            display("Post-upload READY MCU status (old running image; package identity match pending execution):", observed, out)
            print("Press the MCU reset button.", file=out, flush=True)
            waiting_reset = True
        if state in ("TRIAL", "CONFIRMED") and not report_checked:
            # Fail closed on missing/invalid report, rather than trusting prose.
            # Unsupported/malformed reports fail immediately; only disconnects
            # may be retried while waiting for the physical reset.
            try:
                flow.transport.connect()
                flow.transport.binary()
                application, message, reported = boot_report(flow.transport.retr("boot-report"))
                current = flow.read()
            except ProtocolError:
                raise
            except OSError:
                if not waiting_reset and clock() >= end:
                    raise
                sleep(poll_interval)
                continue
            forward_snapshot(previous, reported)
            forward_snapshot(reported, current)
            require_healthy(reported)
            require_healthy(current)
            current_state = flow.classify(current)
            if (application != APPLICATIONS[artifact.slot] or
                    current_state not in ("TRIAL", "CONFIRMED") or
                    current["trial_id"] == "none" or int(current["trial_id"]) != flow.trial_id or
                    reported["fault"] != "0" or current["fault"] != "0"):
                raise ValueError("boot-report does not match authoritative uploaded image/trial")
            previous = current
            print("Application report: " + message, file=out)
            report_checked = True
            state = current_state
            waiting_reset = False
            end = clock() + deadline
        if state == "TRIAL" and report_checked and not confirmation_sent:
            if clock() >= end:
                raise ValueError("pre-confirm reads unavailable; no confirmation attempted")
            # A fresh read and health check precede each definitely-unsent retry.
            # Once STOR is entered, even a disconnect must not permit repetition.
            def validate_preconfirm(current):
                nonlocal previous
                forward_snapshot(previous, current)
                require_healthy(current)
                previous = current

            result = flow.confirm(strict=True, validate=validate_preconfirm)
            confirmation_sent = flow.confirm_attempted
            if result == "CONFIRMED":
                state = result
            if confirmation_sent:
                end = clock() + deadline
        if state == "CONFIRMED" and report_checked:
            print(f"SUCCESS: durable CONFIRMED slot={artifact.slot} hash={artifact.digest} "
                  f"release={artifact.release} trial_id={flow.trial_id}", file=out)
            return
        if confirmation_sent and state != "CONFIRMED" and clock() >= end:
            raise ValueError("confirmation outcome unknown; inspect status; do not reupload")
        if not waiting_reset and clock() >= end:
            raise ready_failure("READY/trial not established; inspect status; do not reupload")
        sleep(poll_interval)


def positive_seconds(text):
    value = float(text)
    if not math.isfinite(value) or not 0 < value <= 120:
        raise argparse.ArgumentTypeError("must be >0 and <=120 seconds")
    return value


def main(argv=None, *, transport_factory=FTPTransport, clock=time.monotonic,
         sleep=time.sleep, stdout=None, stderr=None):
    out, err = stdout or sys.stdout, stderr or sys.stderr
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("status", "download"):
        child = sub.add_parser(command)
        if command == "download":
            child.add_argument("directory", type=Path)
        child.add_argument("--host", default="192.168.7.2", help="numeric IPv4 address")
        child.add_argument("--port", type=int, default=21)
        child.add_argument("--user", default="anonymous")
        child.add_argument("--password", default="bench@")
        child.add_argument("--call-timeout", type=positive_seconds, default=30)
        child.add_argument("--deadline", type=positive_seconds, default=75,
                           help="per-operation and pre-reset/confirmation deadline in seconds")
        child.add_argument("--poll-interval", type=positive_seconds, default=1)
    args = parser.parse_args(argv)
    transport = None
    try:
        packages = load_directory(args.directory) if args.command == "download" else None
        transport = transport_factory(args.host, args.port, args.user, args.password,
                                      call_timeout=args.call_timeout,
                                      operation_timeout=args.deadline, monotonic=clock)
        flow = Workflow(transport, sleep)
        if args.command == "status":
            display("MCU status (authenticated running image identity):", flow.read(), out)
        else:
            download(flow, packages, out=out, sleep=sleep, clock=clock,
                     poll_interval=args.poll_interval, deadline=args.deadline)
        return 0
    except KeyboardInterrupt:
        print("Stopped polling; device state unchanged by cancellation. Read status; do not reupload.", file=err)
        return 130
    except (ValueError, UnicodeError) as exc:
        print(f"demo: {exc}; no success established", file=err)
        return 2
    except OSError:
        print("demo: FTP operation failed (check endpoint, MCU/report support and status); "
              "outcome may be unknown; do not reupload", file=err)
        return 4
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    sys.exit(main())
