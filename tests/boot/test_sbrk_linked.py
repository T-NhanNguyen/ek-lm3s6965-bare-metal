"""Run fixed native cases, then replay real GNU-linked addresses under ASan/UBSan.

Only checked-in fixture sources are compiled. No target image is executed and
no synthetic address is mapped/dereferenced. The oracle is the GNU ELF symbol
contract plus independent expected SRAM constants, not allocator output.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_layout import ROOT, SCRIPTS, elf_tables


def run(command):
    subprocess.run([str(arg) for arg in command], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--sanitizers", choices=("ON", "OFF"), required=True)
    parser.add_argument("programs", nargs="+")
    args = parser.parse_args()
    for program in args.programs:
        run([program])
    candidates = sorted((Path.home() / ".toolchains").glob(
        "arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc"))
    candidates += sorted(Path("/Applications/ArmGNUToolchain").glob(
        "*/arm-none-eabi/bin/arm-none-eabi-gcc"))
    gcc = os.environ.get("BOOT_ARM_GCC") or (str(candidates[-1]) if candidates else
                                            shutil.which("arm-none-eabi-gcc"))
    if not gcc:
        raise RuntimeError("installed GNU Arm toolchain required")
    print("Allocator ABI/link compiler: " + gcc, flush=True)
    strict = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
              "-Wconversion", "-Wsign-conversion", "-Wshadow",
              "-Wstrict-prototypes", "-Wmissing-prototypes"]
    arm = [gcc, "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft"]
    with tempfile.TemporaryDirectory(prefix="boot-allocator-") as temporary:
        directory = Path(temporary)
        run([*arm, *strict, "-c", ROOT / "tests/boot/sbrk_abi.c",
             "-o", directory / "abi.o"])
        for key, (script, _, _) in SCRIPTS.items():
            kind = ["-DTEST_GENERIC"] if key != "ftp" else []
            obj = directory / "allocator.o"
            run([*arm, *strict, *kind, "-I" + str(ROOT / "include/lm3s6965"),
                 "-c", ROOT / "tests/boot/sbrk_fixture.c", "-o", obj])
            # .data=16 and .bss=32 plus two allocator words and one errno word:
            # independently expected aligned heap start is 0x20000040.
            for heap_bytes in (0, 32, 8192 if key == "ftp" else 0xdf80):
                elf = directory / "linked.elf"
                run([*arm, "-nostdlib", "-Wl,-z,max-page-size=0x100",
                     "-T" + str(ROOT / script), "-DHEAP_BYTES=" + str(heap_bytes),
                     ROOT / "tests/boot/layout_fixture.S", obj,
                     ROOT / "tests/boot/sbrk_link_stubs.c", "-o", elf])
                _, symbols, _ = elf_tables(elf.read_bytes())
                expected_start = 0x20000040
                expected_base = expected_start + heap_bytes
                expected_end = expected_start + 8192 if key == "ftp" else 0x2000dfc0
                expected = {"_heap_start": expected_start,
                            "_heap_static_end": expected_base, "_heap_end": expected_end,
                            "_diagnostic_start": 0x2000dfc0,
                            "_diagnostic_end": 0x2000e000,
                            "_stack_limit": 0x2000e000, "_estack": 0x20010000}
                for symbol, address in expected.items():
                    if symbols[symbol] != address:
                        raise AssertionError((key, heap_bytes, symbol, hex(symbols[symbol]), hex(address)))
                executable = directory / "native"
                sanitizer = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                             if args.sanitizers == "ON" else [])
                run([args.cc, *strict, *sanitizer, *kind, "-D_DEFAULT_SOURCE",
                     "-DTEST_START=" + hex(symbols["_heap_start"]),
                     "-DTEST_BASE=" + hex(symbols["_heap_static_end"]),
                     "-DTEST_END=" + hex(symbols["_heap_end"]),
                     "-I" + str(ROOT / "include/lm3s6965"),
                     ROOT / "tests/boot/test_sbrk.c", ROOT / "tests/boot/sbrk_fixture.c",
                     ROOT / "tests/boot/sbrk_symbols.S", "-o", executable])
                run([executable])
        print("All five real linker scripts: empty/nonempty/full static heap replay passed")


if __name__ == "__main__":
    main()
