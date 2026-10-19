#!/usr/bin/env python3
"""Narrow offline setup gate. JSONL stdout; full evidence in private runs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
GCC = Path('/opt/homebrew/Cellar/arm-none-eabi-gcc/16.2.0/bin/arm-none-eabi-gcc')
BIN = Path('/opt/homebrew/Cellar/arm-none-eabi-binutils/2.47/bin')
CC = Path('/usr/bin/clang')
CASES = ('gate-order', 'gate-ignored', 'gate-exact', 'arm-closure',
         'retained-watchdog', 'retained-runtime', 'retained-runtime-fault')
TARGETS = dict(zip(CASES[4:], ('test_watchdog', 'test_runtime_watchdog', 'test_runtime_watchdog_fault')))

class Invalid(Exception):
    pass

class Failure(Exception):
    def __init__(self, at, assertion):
        self.at, self.assertion = at, assertion

def main():
    p = argparse.ArgumentParser(description=__doc__)
    select = p.add_mutually_exclusive_group(required=True)
    select.add_argument('--all', action='store_true')
    select.add_argument('--case', action='append', choices=CASES)
    p.add_argument('--human', action='store_true')
    p.add_argument('--failures-only', action='store_true')
    a = p.parse_args()
    if a.case and len(set(a.case)) != len(a.case):
        p.error('duplicate case')
    roster = [c for c in CASES if a.all or c in a.case]
    base = ROOT / '.dev-vault/tests/runs'
    out = base / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '-wdt-setup')
    out.mkdir(parents=True)
    logpath, reportpath = out / 'run.log', out / 'report.jsonl'
    deadline, started = time.monotonic() + 280, time.monotonic()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    records, prepared, stopped = [], set(), None
    # Freeze all local C/header/ASM/CMake/link inputs; no resume.tex access.
    def identity():
        files = set()
        for directory in ('include', 'src', 'tests/boot', 'tests/boot_launch', 'cmake', 'linker'):
            for f in (ROOT / directory).rglob('*'):
                if f.is_file() and (f.suffix in ('.c', '.h', '.S', '.ld', '.cmake', '.py') or f.name == 'CMakeLists.txt'):
                    files.add(f)
        return {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(files)}
    before = identity()
    metadata = {'roster': roster, 'inputs_before': before, 'tools': {}, 'configuration': 'host Release assertions active; ARM Cortex-M3 Thumb soft-float -Os -fno-short-enums'}
    for tool in (GCC, BIN / 'arm-none-eabi-nm', BIN / 'arm-none-eabi-objdump', CC):
        if tool.is_file():
            metadata['tools'][str(tool)] = hashlib.sha256(tool.read_bytes()).hexdigest()
    with logpath.open('wb') as log, reportpath.open('w') as report:
        def emit(r):
            records.append(r)
            line = json.dumps(r, separators=(',', ':'))
            report.write(line + '\n'); report.flush()
            if not a.human and (not a.failures_only or r.get('status') != 'pass'):
                print(line, flush=True)
        def run(argv, binary=False):
            argv = list(map(str, argv))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Invalid('280-second deadline exhausted')
            log.write(('\n$ ' + repr(argv) + '\n').encode()); log.flush()
            start = log.tell()
            try:
                result = subprocess.run(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=remaining)
            except (OSError, subprocess.SubprocessError) as exc:
                raise Invalid(str(exc)) from exc
            log.flush(); end = log.tell()
            with logpath.open('rb') as source:
                source.seek(start); body = source.read(min(end-start, 65536)).decode(errors='replace')
            if binary and re.search(r'(?:ERROR|SUMMARY): AddressSanitizer: stack-overflow\b', body):
                raise Invalid('AddressSanitizer stack-overflow; child stack environment inadequate; see run.log')
            if end-start > 65536:
                raise Invalid('command evidence exceeds 64KiB parser bound; raw retained')
            if result.returncode:
                if binary:
                    m = re.search(r'([^\s:]+\.(?:c|h)):(\d+):\s*([^\n]+)', body)
                    raise Failure(m[1]+':'+m[2] if m else argv[0]+':0', m[3] if m else 'binary exit '+str(result.returncode))
                raise Invalid('command exit '+str(result.returncode)+'; command and full output in run.log')
            return body
        def require(ok, assertion):
            if not ok:
                raise Failure('tests/boot/run_wdt_setup.py:0', assertion)
        def host():
            if 'host' not in prepared:
                run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', '-I', ROOT/'include', ROOT/'tests/boot/wdt_setup_host.c', ROOT/'src/boot_watchdog_target.c', '-o', out/'host'])
                prepared.add('host')
        def arm():
            def decode(text):
                rows = []
                for line in text.splitlines():
                    m = re.fullmatch(r'[ \t]*[0-9a-f]+:[ \t]+(?:[0-9a-f]{4}[ \t]+)+([a-z][a-z0-9.]*)[ \t]*([^\r\n]*)', line)
                    if m:
                        rows.append(m.groups())
                return rows
            require(decode(
                ' 182:\tbf00      \tnop\n'
                ' 184:\tbf00      \tnop\n'
                ' 186:\tbf00      \tnop\n'
                ' 188:\t4770      \tbx\tlr\n'
            ) == [('nop', ''), ('nop', ''), ('nop', ''), ('bx', 'lr')],
                'instruction decoder preserves three operandless NOPs then bx lr')
            flags = [GCC, '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-fno-short-enums', '-std=c11', '-Os', '-ffreestanding', '-fno-common', '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror', '-I', ROOT/'include']
            objects = []
            for i, source in enumerate(('tests/boot/wdt_setup_arm.c', 'src/boot_watchdog_target.c', 'src/boot_startup_gptm.c')):
                obj = out/('arm-'+str(i)+'.o')
                run([*flags, '-c', ROOT/source, '-o', obj]); objects.append(obj)
            nm, objdump, elf = BIN/'arm-none-eabi-nm', BIN/'arm-none-eabi-objdump', out/'boot.elf'
            section_text = run([objdump, '-h', objects[1]])
            symbol_text = run([objdump, '-t', objects[1]])
            sections = {}
            lines = section_text.splitlines()
            for index, line in enumerate(lines):
                m = re.fullmatch(r'\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+2\*\*\d+\s*', line)
                if not m:
                    continue
                if index + 1 >= len(lines) or m[1] in sections:
                    raise Invalid('untrustworthy adapter section table')
                flags = {flag.strip() for flag in lines[index + 1].split(',')}
                sections[m[1]] = (int(m[2], 16), flags)
            if not sections:
                raise Invalid('adapter section table not parsed')
            writable = {name for name, (_, flags) in sections.items()
                        if 'ALLOC' in flags and 'READONLY' not in flags}
            mutable = ['section ' + name for name in sorted(writable) if sections[name][0] != 0]
            symbols_seen = 0
            for line in symbol_text.splitlines():
                if not re.match(r'^[0-9a-fA-F]+\s', line):
                    continue
                m = re.fullmatch(r'[0-9a-fA-F]+ (.{7})\s+(\S+)\s+([0-9a-fA-F]+)\s+(.+)', line)
                if not m:
                    raise Invalid('adapter symbol row not parsed')
                symbols_seen += 1
                flags, section, name = m[1], m[2], m[4]
                if section == '*COM*':
                    mutable.append('common ' + name)
                elif section not in sections and section not in ('*ABS*', '*UND*'):
                    raise Invalid('unknown adapter symbol section: ' + section)
                elif 'O' in flags and section in writable:
                    mutable.append('object ' + name)
            if not symbols_seen:
                raise Invalid('adapter symbol table not parsed')
            require(not mutable, 'adapter must define no writable/static state: ' + ', '.join(mutable))
            run([GCC, '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-nostdlib', '-Wl,--gc-sections,-z,max-page-size=0x100', '-Wl,-Map='+str(out/'boot.map'), '-T', ROOT/'linker/bootloader.ld', *objects, '-o', elf])
            require(not run([nm, '-u', elf]).strip(), 'no undefined boot fixture symbols')
            symbols = {m[3]: int(m[1], 16) for m in re.finditer(r'^([0-9a-fA-F]+)\s+(\S)\s+(\S+)$', run([nm, '-n', elf]), re.M)}
            for name in ('Reset_Handler', 'boot_watchdog_target_clock_ready', 'boot_startup_gptm_direct_read', 'boot_startup_gptm_direct_write', 'boot_startup_gptm_direct_delay'):
                require(name in symbols and 0 <= symbols[name] < 0x8000, 'boot FLASH closure: '+name)
            for start, end in (('_sdata', '_edata'), ('_sbss', '_ebss')):
                require(start in symbols and symbols[start] == symbols.get(end), 'empty fixture storage: '+start)
            rows = decode(run([objdump, '-d', '--disassemble=boot_startup_gptm_direct_delay', elf]))
            require(len(rows) >= 4 and all(op == 'nop' for op, _ in rows[:-1]) and rows[-1][0] == 'bx' and rows[-1][1].strip() == 'lr', 'closed direct delay >=3 NOPs then bx lr')
        def retained():
            if 'retained' not in prepared:
                cmake = shutil.which('cmake')
                if not cmake:
                    raise Invalid('cmake unavailable')
                run([cmake, '-S', ROOT/'tests/boot', '-B', out/'host-build', '-DCMAKE_C_COMPILER='+str(CC), '-DCMAKE_BUILD_TYPE=Release', '-DBOOT_SANITIZERS=ON'])
                run([cmake, '--build', out/'host-build', '--parallel', '2', '--target', *[TARGETS[c] for c in roster if c in TARGETS]])
                prepared.add('retained')
        for case in roster:
            if stopped:
                emit({'case': case, 'status': 'invalid', 'reason': 'not attempted after infrastructure failure: '+stopped[:160]}); continue
            try:
                if case.startswith('gate-'):
                    host(); run([out/'host', case], binary=True)
                elif case == 'arm-closure':
                    arm()
                else:
                    retained()
                    executable = out/'host-build'/('' if case == 'retained-watchdog' else 'boot_launch')/TARGETS[case]
                    run([executable], binary=True)
                emit({'case': case, 'status': 'pass'})
            except Failure as exc:
                emit({'case': case, 'status': 'fail', 'at': exc.at, 'assert': exc.assertion[:240]})
            except Exception as exc:
                stopped = str(exc)
                emit({'case': case, 'status': 'invalid', 'reason': stopped[:240]})
        after = identity()
        metadata.update(inputs_after=after, inputs_unchanged=(before == after), elapsed_seconds=time.monotonic()-started, complete=not stopped)
        if before != after:
            # Preserve observed failures; identity invalidity is a separate execution precondition.
            emit({'case': 'input-identity', 'status': 'invalid', 'reason': 'scoped inputs changed during execution'})
        counts = {s: sum(r.get('status') == s for r in records) for s in ('pass', 'fail', 'invalid')}
        metadata['counts'] = counts
        (out/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
        emit({'summary': {'target': 'wdt-setup', 'kind': 'python', **counts, 'report': str(reportpath), 'log': str(logpath)}})
        emit({'rollup': {**counts, 'targets': 1}})
    for old in sorted(base.glob('*-wdt-setup'))[:-5]:
        if old.is_dir() and not old.is_symlink():
            shutil.rmtree(old)
    if a.human:
        with logpath.open() as source:
            shutil.copyfileobj(source, os.sys.stdout)
        print(reportpath.read_text(), end='')
    return int(bool(counts['fail'] or counts['invalid']))

if __name__ == '__main__':
    raise SystemExit(main())
