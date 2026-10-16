#!/usr/bin/env python3
"""Offline HOST integrity entry point. Stdout is JSONL; verbose evidence is ignored.

Use --all --budget-seconds 900, or repeat --case target:name. No automatic retry.
Existing shell/CMake entry points remain available as raw, non-JSONL interfaces.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("manifest.json")
RUNS = ROOT / ".dev-vault/tests/runs"
PYDIR = ROOT / "tests/ethernet/platform"


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True)


def bounded(value):
    return " ".join(str(value).split())[:300]


def identity_excluded(name):
    """Do not read private/local content, even if it becomes nonignored/tracked.

    Status still records drift; this is a content-read boundary, not permission
    to publish excluded paths. Offline QEMU fixtures remain public inputs.
    """
    path = Path(name)
    return (path.name == "resume.tex" or
            any(part in (".dev-vault", ".pi", ".git", "__pycache__")
                for part in path.parts) or path.suffix == ".pyc" or
            any(part == "build" or part.startswith("build-")
                for part in path.parts[:-1]))


def identity():
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=ROOT)
    files = git("ls-files", "--cached", "--others", "--exclude-standard", "-z")
    hashes = {}
    for name in files.decode().split("\0"):
        if not name or identity_excluded(name):
            continue
        path = ROOT / name
        # Hash link identity, never follow a link into excluded/private content.
        if path.is_symlink():
            body = os.fsencode(os.readlink(path))
            hashes[name] = "symlink:" + hashlib.sha256(body).hexdigest()
        elif path.is_file():
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"head": git("rev-parse", "HEAD").decode().strip(),
            "status": git("status", "--short").decode(),
            "submodules": git("submodule", "status", "--recursive").decode(),
            "files": hashes}


class Invalid(RuntimeError):
    pass


class Run:
    def __init__(self, args, selected):
        self.args, self.selected = args, selected
        self.start = time.monotonic()
        self.deadline = self.start + args.budget_seconds
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
                        ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0",
                        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        self.base = RUNS / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-host-integrity")
        self.base.mkdir(parents=True)
        (self.base / ".harness-owned").touch()
        self.logpath, self.reportpath = self.base / "run.log", self.base / "report.jsonl"
        self.log = self.logpath.open("w", buffering=1)
        self.report = self.reportpath.open("w", buffering=1)
        self.records = []
        self.active = None
        self.initial = None
        self.build = self.base / "build"
        self.native = self.base / "native"

    def command(self, argv, cwd=ROOT, env=None):
        if time.monotonic() >= self.deadline:
            raise Invalid("overall execution budget exhausted")
        self.log.write("\n$ " + " ".join(map(str, argv)) + "\n")
        # Disk capture, never a pipe: logs cannot block or inflate model context.
        offset = self.log.tell()
        try:
            self.active = subprocess.Popen(list(map(str, argv)), cwd=cwd,
                                           env=env or self.env, stdout=self.log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            try:
                rc = self.active.wait(timeout=max(.001, self.deadline - time.monotonic()))
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                os.killpg(self.active.pid, signal.SIGKILL)
                self.active.wait()
                raise Invalid("budget exhausted or interrupted; active process group killed")
        except OSError as exc:
            raise Invalid(bounded(exc)) from exc
        finally:
            self.active = None
        self.log.flush()
        # Parsing here is machine-only. Only JSONL digests are consumed by the gate.
        with self.logpath.open() as stream:
            stream.seek(offset)
            text = stream.read()
        return rc, text

    def require(self, argv, cwd=ROOT, env=None):
        rc, text = self.command(argv, cwd, env)
        if rc:
            raise Invalid(f"setup command failed exit {rc}: {bounded(argv)}; see run.log")
        return text

    def emit(self, obj):
        line = compact(obj)
        self.report.write(line + "\n")
        if not self.args.failures_only or obj.get("status") != "pass":
            print(line, flush=True)

    def record(self, name, status, detail=""):
        obj = {"case": name, "status": status}
        if status == "invalid":
            obj["reason"] = bounded(detail)
        elif status == "fail":
            # Assertion source is evidence, not a guessed production location.
            match = re.search(r"(?:[\w./-]+\.(?:c|h|py):\d+)", detail)
            obj["at"] = match.group() if match else str(self.logpath) + ":1"
            obj["assert"] = bounded(detail)
        self.records.append(obj)
        self.emit(obj)

    def prepare(self):
        self.initial = identity()
        (self.base / "provenance-before.json").write_text(json.dumps(self.initial, indent=2) + "\n")
        (self.base / "plan.json").write_text(json.dumps({"implementation": self.initial["head"],
            "dirty_state": self.initial["status"], "cases": self.selected,
            "budget_seconds": self.args.budget_seconds, "build_commands": 2,
            "retries": 0, "semantic_additions": 0}, indent=2) + "\n")
        for tool in ("cmake", "ctest", "cc", "qemu-system-arm"):
            if not shutil.which(tool):
                raise Invalid("required offline tool missing: " + tool)
        if not (shutil.which("arm-none-eabi-gcc") or os.environ.get("BOOT_ARM_GCC") or
                list((Path.home() / ".toolchains").glob("*/bin/arm-none-eabi-gcc"))):
            raise Invalid("required Arm compiler missing")
        for path in ("third_party/lwip/src/include/lwip/init.h", "tests/boot/test_workflow.py"):
            if not (ROOT / path).is_file():
                raise Invalid("fixture missing: " + path)
        self.require(["cmake", "--version"])
        self.require(["ctest", "--version"])
        self.require(["cmake", "-S", ROOT / "tests/boot", "-B", self.build,
                      "-DCMAKE_BUILD_TYPE=Release", "-DBOOT_SANITIZERS=ON",
                      "-DCMAKE_C_COMPILER=/usr/bin/cc", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
        self.require(["cmake", "--build", self.build, "--parallel", "4"])
        commands = json.loads((self.build / "compile_commands.json").read_text())
        for item in commands:
            if not item["file"].endswith(".c"):
                continue
            cmd = item["command"]
            if "-fsanitize=address,undefined" not in cmd:
                raise Invalid("uninstrumented host C translation unit: " + item["file"])
            if ".dir/" in cmd and ("/test_" in item["file"] or item["file"].endswith("/package_verify.c")):
                if "-UNDEBUG" not in cmd or cmd.rfind("-DNDEBUG") > cmd.rfind("-UNDEBUG"):
                    raise Invalid("disabled fixture assertion: " + item["file"])
        raw = self.require(["ctest", "--test-dir", self.build, "--show-only=json-v1"])
        roster = json.loads(raw[raw.index("{"):])["tests"]
        if (len(roster) != len(ALL_CASES["ctest"]) or
                {t["name"] for t in roster} != set(ALL_CASES["ctest"])):
            raise Invalid("CTest roster mismatch against explicit35 manifest")
        for test in roster:
            if not test.get("command") or not Path(test["command"][0]).is_file():
                raise Invalid("unreachable CTest command: " + test["name"])
            props = {p["name"]: p["value"] for p in test.get("properties", [])}
            if props.get("DISABLED"):
                raise Invalid("disabled CTest: " + test["name"])
        (self.base / "ctest-roster.json").write_text(json.dumps(roster, indent=2) + "\n")
        if any(name.startswith("ethernet:") for name in self.selected):
            env = dict(self.env, CC="/usr/bin/cc", FTP_TEST_BUILD_ONLY="1",
                       FTP_TEST_CONFIG="Release", FTP_TEST_BUILD_DIR=str(self.native),
                       FTP_BOOT_BUILD_DIR=str(self.build))
            # No inherited sanitizer escape hatch; the shell keeps its legacy API.
            env.pop("FTP_PLATFORM_TEST_FLAGS", None)
            self.require(["bash", ROOT / "scripts/ethernet/test.sh", "core", "tcp", "lwip", "platform"], env=env)
            if any(not (self.native / n).is_file() for n in ALL_CASES["ethernet"]):
                raise Invalid("native binary preparation incomplete")
        if identity() != self.initial:
            raise Invalid("source/worktree changed during bring-up")

    def case(self, name):
        if identity() != self.initial:
            raise Invalid("source/worktree drift before case")
        target, case = name.split(":", 1)
        if target == "ctest":
            xml = self.base / (case + ".xml")
            rc, text = self.command(["ctest", "--test-dir", self.build, "-R", "^" + re.escape(case) + "$",
                                     "--verbose", "--test-output-size-passed", "10485760",
                                     "--test-output-size-failed", "10485760", "--output-junit", xml])
            if not xml.exists():
                raise Invalid("CTest did not produce JUnit evidence: " + case)
            children = list(ET.parse(xml).getroot().iter("testcase"))
            if len(children) != 1 or children[0].get("name") != case:
                raise Invalid("CTest result roster mismatch: " + case)
            child = children[0]
            text += "\n" + "".join(child.itertext())
            if "test output was removed" in text:
                raise Invalid("CTest evidence exceeded capture limit: " + case)
            if child.find("skipped") is not None or re.search(r"OK \(skipped=|\.\.\. skipped |UNSUPPORTED", text):
                raise Invalid("skipped/unsupported coverage inside " + case)
        elif target == "ethernet":
            rc, text = self.command([self.native / case])
        else:
            rc, text = self.command([sys.executable, "-m", "unittest", "-v", case], cwd=PYDIR)
            if rc == 0 and not re.search(r"Ran 1 test\b", text):
                raise Invalid("Python method did not produce one-case evidence")
            if re.search(r"OK \(skipped=|\.\.\. skipped ", text):
                raise Invalid("Python method skipped")
        if re.search(r"FileNotFoundError|ModuleNotFoundError|No such file or directory|NOT_BUILT|Unable to find executable", text):
            raise Invalid("missing fixture/tool during " + name + "; see run.log")
        if identity() != self.initial:
            raise Invalid("source/worktree drift during case")
        if target == "ctest" and case == "boot_pc_workflow" and rc == 0:
            if not re.search(r"Ran 23 tests\b", text):
                raise Invalid("nested workflow did not establish all23 method outcomes")
        if rc:
            lines = text.splitlines()
            evidence = next((line for line in lines if re.search(
                r"AssertionError|Assertion .*failed|assertion.*failed|CHECK.*failed|runtime error:|ERROR: AddressSanitizer", line)),
                f"process exit {rc}; exact output in run.log and target JUnit")
            self.record(name, "fail", evidence)
        else:
            self.record(name, "pass")

    def finish(self, blocker):
        if self.initial is not None:
            after = identity()
            (self.base / "provenance-after.json").write_text(json.dumps(after, indent=2) + "\n")
            if after != self.initial:
                blocker = "source/worktree drift invalidates completed evidence"
                # Do not rewrite emitted records; explicit validity ledger withdraws credit.
                (self.base / "INVALIDATED").write_text(blocker + "\n")
        completed = {r["case"] for r in self.records}
        for name in self.selected:
            if name not in completed:
                self.record(name, "invalid", blocker or "not executed")
        for target in dict.fromkeys(n.split(":", 1)[0] for n in self.selected):
            counts = Counter(r["status"] for r in self.records if r["case"].startswith(target + ":"))
            self.emit({"summary": {"target": target, "kind": target,
                "pass": counts["pass"], "fail": counts["fail"], "invalid": counts["invalid"],
                "report": str(self.reportpath), "log": str(self.logpath)}})
        totals = Counter(r["status"] for r in self.records)
        self.emit({"rollup": {"pass": totals["pass"], "fail": totals["fail"],
                              "invalid": totals["invalid"],
                              "targets": len({n.split(":", 1)[0] for n in self.selected})}})
        (self.base / "execution.json").write_text(json.dumps({"seconds": time.monotonic() - self.start,
            "budget_seconds": self.args.budget_seconds, "blocker": blocker,
            "valid": not (self.base / "INVALIDATED").exists(), "counts": dict(totals)}, indent=2) + "\n")
        self.report.close()
        self.log.close()
        # Restrict deletion to ignored, marked artifacts owned by this runner.
        owned = sorted(p for p in RUNS.glob("*-host-integrity") if (p / ".harness-owned").is_file())
        for old in owned[:-5]:
            shutil.rmtree(old)
        if self.args.human:
            # --human is explicit opt-in; normal stdout remains JSONL only.
            sys.stderr.write(self.logpath.read_text())
        return int(bool(totals["fail"] or totals["invalid"] or blocker))


DATA = json.loads(MANIFEST.read_text())
ALL_CASES = {target: [row[0] for row in rows] for target, rows in DATA["targets"].items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    select = parser.add_mutually_exclusive_group(required=True)
    select.add_argument("--all", action="store_true")
    select.add_argument("--case", action="append", default=[])
    parser.add_argument("--budget-seconds", type=float, required=True)
    parser.add_argument("--human", action="store_true")
    parser.add_argument("--failures-only", action="store_true")
    args = parser.parse_args()
    roster = [target + ":" + name for target, names in ALL_CASES.items() for name in names]
    selected = [n for n in roster if n not in DATA["merged_cases"]] if args.all else args.case
    if not 0 < args.budget_seconds <= 900:
        parser.error("budget must be positive and at most900 seconds")
    if len(set(selected)) != len(selected) or any(n not in roster for n in selected):
        parser.error("duplicate or unknown case; see manifest.json")
    if len(roster) != 54 or len(set(roster)) != 54 or any(len(row) != 3 or not all(row) for rows in DATA["targets"].values() for row in rows):
        parser.error("invalid54-case intent manifest")
    # Before artifact creation: never put verbose logs in an unignored directory.
    if subprocess.run(["git", "check-ignore", "-q", ".dev-vault/"], cwd=ROOT).returncode:
        parser.error(".dev-vault/ must be gitignored")
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    run = Run(args, selected)
    blocker = ""
    try:
        run.prepare()
        for name in selected:
            run.case(name)
    except (Invalid, KeyboardInterrupt, OSError, ValueError, ET.ParseError) as exc:
        blocker = bounded(exc) or "interrupted"
    return run.finish(blocker)


if __name__ == "__main__":
    raise SystemExit(main())
