#!/usr/bin/env python3
"""PC restricted-bench FTP client. No production channel authentication."""
import argparse
import getpass
import os
from pathlib import Path
import stat
import sys
import time

from ftp_transport import FTPTransport, DeadlineFTP, validate
from workflow import Artifact, ConfirmedIdentity, Workflow

EXIT = {"CONFIRMED": 0, "STATUS": 0, "TRIAL": 3, "UNKNOWN": 4,
        "MISMATCH": 5, "UNHEALTHY": 6}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # argparse messages can include supplied values. Do not echo them.
        self.exit(2, "client: invalid arguments; use --help\n")


def bounded_int(text):
    value = int(text)
    if not 1 <= value <= 120:
        raise ValueError
    return value


def retained_id(text):
    if not text.isascii() or not text.isdecimal() or len(text) > 10:
        raise ValueError
    value = int(text)
    if value > 0xffffffff:
        raise ValueError
    return value


def load(path):
    # Do not open FIFOs/devices. Recheck the opened descriptor and bound read.
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("regular package file required")
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or not 396 <= info.st_size <= 111776:
            raise ValueError("package size outside limits")
        body = stream.read(111777)
    return Artifact.from_bytes(body)


def main(argv=None, *, ftp_factory=DeadlineFTP, environ=None,
         password_prompt=getpass.getpass, sleep=time.sleep, stdout=None, stderr=None):
    out, err = stdout or sys.stdout, stderr or sys.stderr
    env = os.environ if environ is None else environ
    parser = Parser(description=__doc__)
    parser.add_argument("--host", required=True, help="numeric IPv4 address; no DNS")
    parser.add_argument("--port", type=int, default=21)
    parser.add_argument("--user", default=env.get("BOOT_FTP_USER", "anonymous"))
    parser.add_argument("--prompt-password", action="store_true")
    parser.add_argument("--call-timeout", type=bounded_int, default=5)
    parser.add_argument("--operation-timeout", type=bounded_int, default=75)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    update = sub.add_parser("update")
    update.add_argument("--slot-a", type=Path, required=True)
    update.add_argument("--slot-b", type=Path, required=True)
    update.add_argument("--polls", type=bounded_int, default=60)
    update.add_argument("--confirm", action="store_true")
    resolve = sub.add_parser("resolve")
    resolve.add_argument("--package", type=Path, required=True)
    resolve.add_argument("--trial-id", type=retained_id, required=True)
    resolve.add_argument("--polls", type=bounded_int, default=60)
    args = parser.parse_args(argv)
    transport = None
    try:
        password = (password_prompt("FTP password: ") if args.prompt_password else
                    env.get("BOOT_FTP_PASSWORD", "bench@"))
        validate(args.host, args.port, args.user, password,
                 args.call_timeout, args.operation_timeout)
        artifacts = None
        if args.command == "update":
            artifacts = {"A": load(args.slot_a), "B": load(args.slot_b)}
            if any(a.slot != slot for slot, a in artifacts.items()):
                raise ValueError("package target mismatch")
        elif args.command == "resolve":
            selected = load(args.package)
        transport = FTPTransport(args.host, args.port, args.user, password,
                                 ftp_factory=ftp_factory, call_timeout=args.call_timeout,
                                 operation_timeout=args.operation_timeout)
        flow = Workflow(transport, sleep)
        if args.command == "status":
            values = flow.read()
            print("".join(f"{key}={value}\n" for key, value in values.items()), end="", file=out)
            return EXIT["STATUS"]
        if args.command == "resolve":
            flow = ConfirmedIdentity(transport, selected, args.trial_id, sleep)
        else:
            flow.upload({slot: a.body for slot, a in artifacts.items()})
        result = flow.poll(args.polls)
        if args.command == "update" and args.confirm and result == "TRIAL":
            result = flow.confirm()
            if result == "UNKNOWN":
                result = flow.poll(args.polls)
        print(result, file=out)
        return EXIT[result]
    except (OSError, EOFError):
        print("UNKNOWN", file=out)
        print("client: operation failed; no success established", file=err)
        return EXIT["UNKNOWN"]
    except (ValueError, UnicodeError):
        print("client: invalid input or status; no success established", file=err)
        return 2
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    sys.exit(main())
