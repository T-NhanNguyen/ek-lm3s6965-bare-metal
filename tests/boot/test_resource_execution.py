"""Bounded isolated Cortex-M3 QEMU resource gate; unsupported is never a pass."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from resource_probe.check import inspect, check_map, require
import test_layout

ROOT = test_layout.ROOT
FIELDS = set("test result expected peak live count_peak count calls failed backing break sbrk_failed increment capacity stack before guards msp output tracking status".split())
ERRORS = [0, 0, 0, 0, 2, 4, 8, 9, 6, 7, 5]


def parse(text, returncode, case, old_ftp_2k=False):
    require(len(text) < 8192, "oversized emulator output")
    lines = text.strip().splitlines()
    # No ignored crash/warning text or duplicate/missing reports.
    require(len(lines) == 1 and lines[0].startswith("RESOURCE "), "missing/extra report")
    words = lines[0][9:].split()
    require(all(re.fullmatch(r"[a-z_]+=[0-9]{1,10}", w) for w in words), "malformed report")
    result = dict(w.split("=") for w in words)
    require(len(result) == len(words) and set(result) == FIELDS, "duplicate/missing fields")
    r = {k: int(v) for k, v in result.items()}
    require(all(v <= 0xffffffff for v in r.values()), "report integer overflow")
    require(0 <= case <= 26, "unsupported case")
    kind = case - 3 if case <= 13 else case - 10 if case <= 20 else (6, 8, 9)[(case - 21) % 3]
    expected = 0 if case < 3 else ERRORS[kind]
    if old_ftp_2k and case >= 3 and kind != 4:
        expected = 5
    status = int(case == 1)  # ONLY deliberate termination calibration may exit 1.
    require(r["test"] == case and r["result"] == r["expected"] == expected, "wrong verifier error")
    require(returncode == r["status"] == status, "unexpected emulator exit")
    require(r["live"] == r["count"] == r["tracking"] == 0, "leak/bookkeeping failure")
    require(r["guards"] == r["output"] == 1, "guard/output failure")
    require(0x2000e010 <= r["msp"] <= 0x20010000, "not real protected MSP")
    require(0 <= r["before"] <= r["stack"] < 8192 - 16, "stack overflow")
    require(r["break"] <= r["backing"] <= r["capacity"], "backing overflow")
    require(r["count_peak"] <= 128 and r["peak"] <= r["backing"], "invalid peak")
    if case < 3:
        require(r["stack"] >= r["before"] + 512, "stack calibration failed")
        require(r["peak"] == (110 if case == 2 else 0), "allocation calibration failed")
        require(r["count_peak"] == (2 if case == 2 else 0), "count calibration failed")
    elif kind == 10:
        require(r["failed"] == 1 and r["calls"] == 1 and r["peak"] == 0, "fault calibration failed")
    elif old_ftp_2k and kind != 4:
        require(r["failed"] > 0 and r["sbrk_failed"] > 0 and r["increment"] > 0
                and r["break"] + r["increment"] > r["capacity"], "no genuine FTP OOM proof")
    else:
        require(r["failed"] == r["sbrk_failed"] == 0, "unexpected allocator failure")
        if kind != 4:
            require(r["calls"] > 0 and r["count_peak"] > 0, "crypto not executed")
    return r


def execute(qemu, elf, case, old_ftp_2k):
    try:
        p = subprocess.run([qemu, "-M", "lm3s6965evb", "-nographic", "-monitor", "none",
                            "-serial", "none", "--semihosting-config",
                            f"enable=on,target=native,arg={case}", "-kernel", str(elf)],
                           capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"QEMU timeout (case {case})") from error
    return parse(p.stdout + p.stderr, p.returncode, case, old_ftp_2k)


def disassembly_gate(gcc, elf):
    objdump = subprocess.check_output([gcc, "-print-prog-name=objdump"], text=True).strip()
    text = subprocess.check_output([objdump, "-d", str(elf)], text=True)
    def body(name):
        match = re.search(r"<" + name + r">:\n(.*?)(?=\n\n)", text, re.S)
        require(match is not None, "missing disassembly: " + name)
        return match[1]
    scanner = body("stack_depth")
    require(not re.search(r"\b(push|pop|str|strd|stm|bl|blx)\b|\bsp\b", scanner), "scanner uses stack/stores/calls")
    painter = body("FTP_Stack_Reset_Handler")
    require("Reset_Handler" in painter and "str.w" in painter and "primask" in painter.lower()
            and not re.search(r"\b(push|pop|bl|blx)\b|\bsp\b", painter), "invalid reset painter")
    for caller, callee in (("__wrap_calloc", "calloc"), ("__wrap_free", "free"),
                           ("__wrap__sbrk", "_sbrk"), ("_sbrk_r", "__wrap__sbrk")):
        require("<" + callee + ">" in body(caller), "allocator wrapper closure: " + caller)
    # Reset-painter literal pool establishes exact stack-only stores.
    require("2000e000" in painter and "20010000" in painter and "a5c39e71" in painter,
            "wrong painting interval/pattern")
    return text


def instrumentation_ram(gcc, elf):
    nm = subprocess.check_output([gcc, "-print-prog-name=nm"], text=True).strip()
    text = subprocess.check_output([nm, "-S", str(elf)], text=True)
    names = set("slots command report live peak count count_peak calls failed sbrk_failed backing_peak failed_increment tracking_error inject used".split())
    sizes = {}
    for line in text.splitlines():
        words = line.split()
        if len(words) == 4 and words[3] in names:
            require(words[2] == "b", "metadata not static bss")
            sizes[words[3]] = int(words[1], 16)
    require(set(sizes) == names, "missing metadata symbols")
    return {"bytes": sum(sizes.values()), "symbols": sizes}


class ParserTests(unittest.TestCase):
    def test_old_capacity_oom_is_separate_and_requires_real_growth_failure(self):
        values = dict.fromkeys(FIELDS, 0)
        values.update(test=3, result=5, expected=5, peak=448, count_peak=9,
                      calls=17, failed=1, backing=536, **{"break": 536},
                      sbrk_failed=1, increment=4096, capacity=2048,
                      stack=2020, before=476, guards=1, output=1, msp=0x2000fe00)
        text = "RESOURCE " + " ".join(f"{k}={v}" for k, v in values.items()) + "\n"
        parse(text, 0, 3, old_ftp_2k=True)
        with self.assertRaises(ValueError):
            parse(text, 0, 3)  # Production policy must require successful valid verification.
        for old, new in (("sbrk_failed=1", "sbrk_failed=0"),
                         ("increment=4096", "increment=1"), ("failed=1", "failed=0")):
            with self.subTest(old=old), self.assertRaises(ValueError):
                parse(text.replace(old, new), 0, 3, old_ftp_2k=True)

    def test_reject_missing_crash_timeout_nonzero_and_mutations(self):
        values = dict.fromkeys(FIELDS, 0)
        values.update(test=0, stack=1024, before=476, guards=1, output=1,
                      msp=0x2000fe00, capacity=50000)
        text = "RESOURCE " + " ".join(f"{k}={v}" for k, v in values.items()) + "\n"
        parse(text, 0, 0)
        # A timeout must propagate as rejection, never become a report/pass.
        from unittest.mock import patch
        with patch("subprocess.run", side_effect=subprocess.TimeoutExpired("qemu", 20)):
            with self.assertRaisesRegex(ValueError, "QEMU timeout"):
                execute("qemu", Path("unused"), 0, False)
        for bad, code in (("", 0), (text, -6), (text, 1), (text + text, 0),
                          ("fatal: lockup\n" + text, 0), (text.replace("live=0", "live=1"), 0),
                          (text.replace("guards=1", "guards=0"), 0),
                          (text.replace("status=0", "status=1"), 0),
                          (text.replace("result=0", "result=5"), 0),
                          (text.replace("stack=1024", "stack=8192"), 0),
                          (text.replace("calls=0", "calls=9999999999"), 0),
                          (text + " status=0", 0)):
            with self.subTest(bad=bad, code=code), self.assertRaises(ValueError):
                parse(bad, code, 0)


def gate(build, qemu, gcc):
    reports = {}
    baseline = inspect((build / "resource_probe").read_bytes())
    check_map((build / "resource_probe.map").read_text())
    for kind in ("boot", "ftp", "old_ftp_2k"):
        elf = build / f"resource_{kind}_execution"
        artifact = inspect(elf.read_bytes(), execution=True, ftp=kind != "boot",
                           old_ftp_2k=kind == "old_ftp_2k")
        check_map(elf.with_suffix(".map").read_text(), ftp=kind != "boot")
        disassembly_gate(gcc, elf)
        metadata = instrumentation_ram(gcc, elf)
        frames = []
        for path in build.rglob("*.su"):
            if "boot_crypto.dir" in str(path) or f"resource_{kind}_execution.dir" in str(path):
                for line in path.read_text().splitlines():
                    name, size, mode = line.rsplit("\t", 2)
                    require(mode == "static", "non-static source frame: " + name)
                    frames.append(int(size))
        require(frames, "missing source static frames")
        source_frame_allowance = max(frames)
        # Calibrate actual reporting/exit, stack painting, and accounting BEFORE crypto.
        calibration = [execute(qemu, elf, i, kind == "old_ftp_2k") for i in range(3)]
        cases = [execute(qemu, elf, i, kind == "old_ftp_2k") for i in range(3, 27)]
        capacity = 2048 if kind == "old_ftp_2k" else (8192 if kind == "ftp" else artifact["heap_capacity"])
        require(all(r["capacity"] == capacity for r in calibration + cases), "wrong execution capacity")
        if kind != "old_ftp_2k":
            require(max(r["peak"] for r in cases) == 3144 and
                    max(r["backing"] for r in cases) == 4632 and
                    max(r["stack"] for r in cases) == 2364, "changed isolated resource peaks")
        else:
            require(all(r["break"] == 536 and r["increment"] == 4096
                        for r in cases[:4]), "changed historical valid-image OOM reason")
        require(max(r["stack"] for r in cases) + source_frame_allowance < 8192 - 16,
                "no conservative source-frame stack margin")
        reports[kind] = {"artifact": artifact, "calibration": calibration, "cases": cases,
                         "instrumentation_ram": metadata,
                         "source_frame_allowance": source_frame_allowance,
                         "flash_delta": artifact["flash_end"] - baseline["flash_end"],
                         "static_ram_delta": artifact["data"] + artifact["bss"] - baseline["data"] - baseline["bss"]}
    return {"baseline": baseline, "measurements": reports}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, help="reuse an explicitly built Release execution directory")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    qemu = os.environ.get("BOOT_QEMU") or shutil.which("qemu-system-arm")
    if not qemu and Path("/opt/homebrew/bin/qemu-system-arm").exists():
        qemu = "/opt/homebrew/bin/qemu-system-arm"
    if not qemu:
        print("UNSUPPORTED: qemu-system-arm unavailable; resource execution NOT validated")
        raise SystemExit(77)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ParserTests)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    test_layout.LayoutTests.setUpClass()
    try:
        print(subprocess.check_output([qemu, "--version"], text=True).splitlines()[0], flush=True)
        with tempfile.TemporaryDirectory(prefix="boot-execution-") as directory:
            build = args.build or Path(directory)
            if not args.build:
                subprocess.run(["cmake", "-S", str(ROOT / "tests/boot/resource_probe"), "-B", str(build),
                                "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "cmake/toolchain-lm3s6965.cmake"),
                                "-DCMAKE_C_COMPILER=" + test_layout.LayoutTests.gcc,
                                "-DCMAKE_BUILD_TYPE=Release", "-DRESOURCE_EXECUTION=ON"], check=True, timeout=30)
                subprocess.run(["cmake", "--build", str(build), "--parallel", "8"], check=True, timeout=60)
            result = gate(build, qemu, test_layout.LayoutTests.gcc)
            text = json.dumps(result, indent=2)
            if args.json:
                args.json.write_text(text + "\n")
            print(text, flush=True)
    finally:
        test_layout.LayoutTests.tearDownClass()
