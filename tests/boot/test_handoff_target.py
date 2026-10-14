"""Fixture-only Cortex-M3 terminal entry; no physical board/peripheral claim."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE_ROOT = Path('/tmp/boot-handoff-target')


def require(condition, message):
    # These gates must survive python -O and PYTHONOPTIMIZE.
    if not condition:
        raise RuntimeError(message)


def instructions(text, symbol):
    """Read this fixture's GNU objdump instruction rows, independent of tabs."""
    match = re.search(r'^\s*[0-9a-f]+ <' + re.escape(symbol) + r'>:\s*\n'
                      r'(.*?)(?=^\s*[0-9a-f]+ <|^Disassembly of section |\Z)',
                      text, re.M | re.S)
    require(match is not None, 'missing disassembly symbol: ' + symbol)
    rows = []
    for line in match.group(1).splitlines():
        if not line.strip():
            continue
        row = re.fullmatch(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{4,8}\s+)+'
                           r'([.a-z][.a-z0-9]*)\s*(.*?)\s*', line)
        require(row is not None, 'unparsed ' + symbol + ' row: ' + line)
        operands = row[3].split('@', 1)[0].strip().lower()
        operands = re.sub(r'\s+', ' ', operands)
        opcode = re.sub(r'\.[nw]$', '', row[2])
        rows.append((int(row[1], 16), opcode, operands))
    require(rows, 'empty disassembly symbol: ' + symbol)
    return rows


def check_sequence(rows, expected, label):
    # An exact allowlist rejects *all* extra memory/control-flow instructions,
    # not just selected ldr/str/branch spellings. It also enforces ISB adjacency.
    require(len(rows) == len(expected), label + ': unexpected instruction count')
    for (_, opcode, operands), (want_opcode, want_operands) in zip(rows, expected):
        require(opcode == want_opcode and re.fullmatch(want_operands, operands),
                label + ': unexpected instruction: ' + opcode + ' ' + operands)


def check_disassembly(text):
    terminal = instructions(text, 'handoff_terminal')
    check_sequence(terminal, [
        ('dsb', 'sy'), ('ldr', r'r3, \[pc, #[0-9]+\]'),
        ('str', r'r0, \[r3, #0\]'), ('dsb', 'sy'), ('isb', 'sy'),
        ('addw', r'r3, pc, #[0-9]+'), ('orr', 'r3, r3, #1'),
        ('mov', 'lr, r3'), ('movs', 'r3, #0'), ('msr', 'msp, r1'),
        ('isb', 'sy'), ('msr', 'control, r3'), ('isb', 'sy'),
        ('msr', 'basepri, r3'), ('msr', 'faultmask, r3'),
        ('msr', 'primask, r3'), ('bx', 'r2'),
    ], 'terminal')
    trap = instructions(text, 'terminal_return_trap')
    check_sequence(trap[:1], [('b', r'[0-9a-f]+ <terminal_return_trap>')], 'trap')
    require(int(trap[0][2].split()[0], 16) == trap[0][0], 'trap must self-loop')
    # GNU assembles ADR here as ADDW with the aligned Thumb PC.
    adr = terminal[5]
    require((((adr[0] + 4) & ~3) + int(adr[2].split('#')[1])) & ~1 == trap[0][0],
            'LR setup must address terminal_return_trap')
    load = terminal[1]
    literal_address = ((load[0] + 4) & ~3) + int(
        re.search(r'#([0-9]+)', load[2])[1])
    require(re.search(r'^\s*' + format(literal_address, 'x') +
                      r':\s+e000ed08\s+\.word\s+0xe000ed08\s*$', text, re.M),
            'terminal literal must be VTOR (0xe000ed08)')

    capture = instructions(text, 'capture')
    # Locate the first call by parsed opcode, never by a literal "bl\\t".
    calls = [i for i, (_, op, _) in enumerate(capture) if op in ('bl', 'blx')]
    require(calls, 'capture must call fixture_application')
    before_c = capture[:calls[0]]
    expected = [('ldr', r'r0, \[pc, #[0-9]+\]')]
    for offset, register in enumerate(('msp', 'control', 'ipsr', 'primask',
                                       'basepri', 'faultmask')):
        expected += [('mrs', 'r1, ' + register),
                     ('str', r'r1, \[r0, #' + str(offset * 4) + r'\]')]
    expected += [('str', r'r2, \[r0, #24\]'), ('str', r'lr, \[r0, #28\]')]
    check_sequence(before_c, expected, 'capture before first call')
    check_sequence(capture[calls[0]:], [
        ('bl', r'[0-9a-f]+ <fixture_application>'),
        ('b', r'[0-9a-f]+ <trap>'),
    ], 'capture call and trap')
    load = capture[0]
    literal_address = ((load[0] + 4) & ~3) + int(
        re.search(r'#([0-9]+)', load[2])[1])
    symbol = re.findall(r'^([0-9a-f]+)\s+g\s+O\s+\.bss\s+'
                        r'[0-9a-f]+\s+captured\s*$', text, re.M)
    require(len(symbol) == 1, 'missing or ambiguous captured data symbol')
    captured_address = int(symbol[0], 16)
    require(re.search(r'^\s*' + format(literal_address, 'x') +
                      r':\s+' + format(captured_address, '08x') +
                      r'\s+\.word\s+0x' + format(captured_address, '08x') +
                      r'\s*$', text, re.M),
            'capture literal must address captured')
    for index, (name, base) in enumerate((('entry_a', 0x8800),
                                         ('entry_b', 0x24000))):
        stub = instructions(text, name)
        check_sequence(stub, [
            ('movs', 'r2, #' + str(index)),
            ('b', format(capture[0][0], 'x') + r' <capture>'),
        ], name)
        require(base + 236 <= stub[0][0] and stub[-1][0] + 4 <= base + 256,
                name + ': outside slot stub')


def run(evidence, *args, timeout=60, output_log=None):
    def record(status, output):
        # Persist output before any failure escapes, including TimeoutExpired's
        # bytes output (even when subprocess was requested in text mode).
        if output_log is not None:
            path = evidence / output_log
            if isinstance(output, bytes):
                path.write_bytes(output)
            else:
                path.write_text(output or '')
        with (evidence / 'commands.log').open('a') as log:
            log.write('command=' + repr(args) + ' timeout=' + str(timeout) +
                      ' status=' + status + ' output=' + repr(output_log) + '\n')

    with (evidence / 'commands.log').open('a') as log:
        log.write('command=' + repr(args) + ' timeout=' + str(timeout) + '\n')
    try:
        result = subprocess.run(args, text=True, errors='replace',
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        record('timeout', error.stdout)
        raise
    except OSError as error:
        record('launch-error: ' + repr(error), '')
        raise
    record('exit=' + str(result.returncode), result.stdout)
    if result.returncode:
        raise RuntimeError('command=' + repr(args) +
                           ' exit=' + str(result.returncode) +
                           ' timeout=' + str(timeout) +
                           ' evidence=' + str(evidence) + '\n' + result.stdout)
    return result.stdout


def main():
    EVIDENCE_ROOT.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='run-', dir=EVIDENCE_ROOT))
    print('Retained evidence: ' + str(evidence), flush=True)
    candidates = sorted((Path.home() / '.toolchains').glob(
        'arm-gnu-toolchain-*-darwin-arm64-arm-none-eabi/bin/'
        'arm-none-eabi-gcc'))
    candidates += sorted((Path.home() / '.toolchains').glob(
        '*/arm-none-eabi/bin/arm-none-eabi-gcc'))
    gcc = os.environ.get('BOOT_ARM_GCC') or (
        str(candidates[-1]) if candidates else
        shutil.which('arm-none-eabi-gcc'))
    qemu = shutil.which('qemu-system-arm')
    if not gcc or not qemu:
        raise RuntimeError('Arm compiler and QEMU required (no skip)')
    objdump = run(evidence, gcc, '-print-prog-name=objdump').strip()
    (evidence / 'tools.log').write_text(
        run(evidence, gcc, '--version') + run(evidence, qemu, '--version'))
    for opt in ('-Os', '-Og'):
        elf = str(evidence / ('fixture' + opt + '.elf'))
        run(evidence, gcc, '-mcpu=cortex-m3', '-mthumb', opt, '-g', '-std=c11',
            '-Wall', '-Wextra', '-Werror', '-Wpedantic',
            '-Wconversion', '-Wsign-conversion', '-Wshadow',
            '-Wstrict-prototypes', '-Wmissing-prototypes',
            '-ffunction-sections', '-fdata-sections',
            '-I' + str(ROOT / 'include'),
            str(ROOT / 'src/boot_watchdog.c'),
            str(ROOT / 'tests/boot/handoff_fixture.c'),
            str(ROOT / 'tests/boot/handoff_fixture.S'),
            str(ROOT / 'tests/boot/handoff_terminal.S'),
            '-nostdlib', '-Wl,--gc-sections',
            '-T' + str(ROOT / 'tests/boot/handoff_fixture.ld'),
            '-lc', '-lgcc', '-o', elf)
        text = run(evidence, objdump, '-d', '-t', elf)
        (evidence / (opt + '.disassembly')).write_text(text)
        check_disassembly(text)
        output = run(evidence, qemu, '-M', 'lm3s6965evb', '-nographic',
                     '-monitor', 'none', '-serial', 'none',
                     '--semihosting-config', 'enable=on,target=native',
                     '-kernel', elf, timeout=15, output_log=opt + '.qemu.log')
        require(output == 'HANDOFF PASS\n', 'unexpected QEMU output: ' + repr(output))
        print(opt + ': 128 shared-machine slot/stack/mask cases, core quiescence, '
              'selected SVC, two stack witness words, watchdog continuity PASS')


if __name__ == '__main__':
    main()
