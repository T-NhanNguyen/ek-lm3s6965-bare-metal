"""No physical device: real Cortex-M3 publication stores / SVC gate in QEMU."""
from pathlib import Path
import re
import os
import shutil
import subprocess
import tempfile

from qemu_output import check_pass_output

ROOT = Path(__file__).resolve().parents[2]

def run(*args):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise RuntimeError(p.stdout)
    return p.stdout

def main():
    candidates = sorted((Path.home() / '.toolchains').glob(
        'arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc'))
    candidates += sorted((Path.home() / '.toolchains').glob('*/arm-none-eabi/bin/arm-none-eabi-gcc'))
    gcc = os.environ.get('BOOT_ARM_GCC') or (str(candidates[-1]) if candidates else shutil.which('arm-none-eabi-gcc'))
    qemu = shutil.which('qemu-system-arm')
    if not gcc or not qemu:
        raise RuntimeError('Arm compiler and QEMU required (no skip)')
    objdump = run(gcc, '-print-prog-name=objdump').strip()
    with tempfile.TemporaryDirectory(prefix='diag-publication-') as directory:
        for opt in ('-Os', '-Og'):
            elf = str(Path(directory) / ('fixture' + opt + '.elf'))
            run(gcc, '-mcpu=cortex-m3', '-mthumb', opt, '-g', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'include'),
                str(ROOT / 'src/boot_diag.c'), str(ROOT / 'src/boot_diag_publication.c'),
                str(ROOT / 'src/boot_snapshot.c'), str(ROOT / 'src/boot_store.c'),
                str(ROOT / 'src/boot_format.c'),
                str(ROOT / 'tests/boot/diag_publication_fixture.c'),
                str(ROOT / 'tests/boot/diag_publication_fixture.S'),
                '-nostdlib', '-Wl,--gc-sections', '-T' + str(ROOT / 'linker/bootloader.ld'),
                '-lc', '-lgcc', '-o', elf)
            text = run(objdump, '-d', elf)
            for token in ('ipsr', 'primask', 'cpsid', 'dmb'):
                assert token in text.lower(), 'missing primitive ' + token
            assert re.search(r'msr\s+PRIMASK', text, re.I), 'mask not restored'
            match = re.search(r'<native_store[^>]*>:\n(.*?)(?=\n\n)', text, re.S)
            assert match, 'missing native aligned word store'
            body = match[1]
            assert re.search(r'\bstr(?:\.w)?\s', body), 'no word STR'
            assert not re.search(r'\b(strb|strh|stm|strd)', body), 'wrong store width'
            assert re.search(r'(lsl #2|lsls[^\n]*#2)', body), 'word indexing not scaled by 4'
            p = subprocess.run([qemu, '-M', 'lm3s6965evb', '-nographic', '-monitor', 'none',
                '-serial', 'none', '--semihosting-config', 'enable=on,target=native',
                '-kernel', elf], capture_output=True, text=True, timeout=15)
            check_pass_output(p.stdout + p.stderr, 'PUBLICATION PASS\n', p.returncode)
            print(opt + ': IPSR/PRIMASK/DMB/aligned STR and LIVE64 boot/adopt/progress/snapshot/SVC (both reads masked/busy) PASS')

if __name__ == '__main__':
    main()
