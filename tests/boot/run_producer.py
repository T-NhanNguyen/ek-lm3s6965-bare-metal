#!/usr/bin/env python3
"""Bounded Timer2 producer gate (no QEMU/hardware). JSONL stdout; full evidence in private runs."""
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
GCC = Path.home()/'.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc'
BIN = GCC.parent
CC = Path('/usr/bin/clang')
CASES = ('producer-success', 'producer-overflow', 'producer-admitted-failure',
         'producer-arm-closure', 'retained-startup', 'retained-clock')

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
    p.add_argument('--budget-seconds', type=int, required=True, choices=range(1,601))
    p.add_argument('--human', action='store_true')
    p.add_argument('--failures-only', action='store_true')
    a = p.parse_args()
    if a.case and len(set(a.case)) != len(a.case):
        p.error('duplicate case')
    roster = [c for c in CASES if a.all or c in a.case]
    base = ROOT / '.dev-vault/tests/runs'
    out = base / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '-timer2-producer')
    out.mkdir(parents=True)
    logpath, reportpath = out / 'run.log', out / 'report.jsonl'
    deadline, started = time.monotonic() + a.budget_seconds, time.monotonic()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='halt_on_error=1:detect_stack_use_after_return=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    records, prepared, stopped = [], set(), None
    # Freeze all local C/header/ASM/CMake/link inputs; no resume.tex access.
    def identity():
        files = set()
        for directory in ('include', 'src', 'tests/boot', 'tests/boot_launch', 'cmake', 'linker', 'tests/ethernet/foreground', 'examples/ethernet-ftp', 'third_party/lwip/src/include'):
            for f in (ROOT / directory).rglob('*'):
                if f.is_file() and (f.suffix in ('.c', '.h', '.S', '.ld', '.cmake', '.py') or f.name == 'CMakeLists.txt'):
                    files.add(f)
        for f in (ROOT/'third_party').glob('*.tar*'):
            files.add(f)
        return {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(files)}
    before = identity()
    metadata = {'roster': roster, 'inputs_before': before, 'tools': {}, 'nested_roster': {'producer-admitted-failure': ['ignored-WDT-gate','active-Timer2','ignored-WDT-LOAD'], 'producer-success': ['setup','modeled-verifier-milestones','real-handoff','startup-through-C_READY']}, 'configuration': 'producer/startup Release ASan+UBSan assertions active; clock Debug; ARM Cortex-M3 Thumb soft-float -Os -fno-short-enums'}
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
        def run(argv, binary=False, raw=False):
            argv = list(map(str, argv))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Invalid('allocated deadline exhausted')
            log.write(('\n$ ' + repr(argv) + '\n').encode()); log.flush()
            start = log.tell()
            try:
                result = subprocess.run(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=remaining)
            except (OSError, subprocess.SubprocessError) as exc:
                raise Invalid(str(exc)) from exc
            log.flush(); end = log.tell()
            if raw:
                if result.returncode:
                    raise Invalid('artifact command exit '+str(result.returncode)+'; see run.log')
                return ''
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
                raise Failure('tests/boot/run_producer.py:0', assertion)
        def host():
            if 'host' not in prepared:
                run(['cmake', '-S', ROOT/'tests/boot', '-B', out/'host-build',
                     '-DCMAKE_C_COMPILER='+str(CC), '-DCMAKE_BUILD_TYPE=Release', '-DBOOT_SANITIZERS=ON'])
                targets = ['test_producer']
                if 'retained-startup' in roster:
                    targets.append('test_startup')
                run(['cmake','--build',out/'host-build','--parallel','2','--target',*targets])
                prepared.add('host')
        def arm():
            if not GCC.is_file() or not (BIN/'arm-none-eabi-objdump').is_file():
                raise Invalid('official GNU Arm 15.3.rel1 bundle unavailable; no PATH fallback')
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
            for i, source in enumerate(('tests/boot/producer_arm.c', 'src/boot_watchdog_target.c', 'src/boot_startup_gptm.c', 'src/boot_startup_producer.c', 'src/boot_startup.c', 'src/boot_watchdog.c', 'src/boot_diag_publication.c', 'src/boot_diag.c')):
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
            run([GCC, '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-nostdlib', '-Wl,--gc-sections,-z,max-page-size=0x100', '-Wl,-Map='+str(out/'boot.map'), '-T', ROOT/'linker/bootloader.ld', *objects, '-Wl,--start-group', '-lc', '-lgcc', '-Wl,--end-group', '-o', elf])
            require(not run([nm, '-u', elf]).strip(), 'no undefined boot fixture symbols')
            symbols = {m[3]: int(m[1], 16) for m in re.finditer(r'^([0-9a-fA-F]+)\s+(\S)\s+(\S+)$', run([nm, '-n', elf]), re.M)}
            for name in ('Reset_Handler', 'boot_watchdog_target_clock_ready', 'boot_startup_gptm_direct_read', 'boot_startup_gptm_direct_write', 'boot_startup_gptm_direct_delay', 'boot_startup_producer_begin', 'boot_startup_producer_begin_io', 'boot_startup_gptm_read', 'boot_watchdog_configure'):
                require(name in symbols and 0 <= symbols[name] < 0x8000, 'boot FLASH closure: '+name)
            for start, end in (('_sdata', '_edata'), ('_sbss', '_ebss')):
                require(start in symbols and symbols[start] == symbols.get(end), 'empty fixture storage: '+start)
            rows = decode(run([objdump, '-d', '--disassemble=boot_startup_gptm_direct_delay', elf]))
            require(len(rows) >= 4 and all(op == 'nop' for op, _ in rows[:-1]) and rows[-1][0] == 'bx' and rows[-1][1].strip() == 'lr', 'closed direct delay >=3 NOPs then bx lr')
            for name in ('boot_startup_gptm_direct_read','boot_startup_gptm_direct_write'):
                leaf = decode(run([objdump,'-d','--disassemble='+name,elf]))
                require(bool(leaf) and all(op in ('ldr','str','movs','bx') for op,_ in leaf)
                        and all(not re.search(r'\b(?:sp|r13)\b', args) for _,args in leaf)
                        and leaf[-1] == ('bx','lr'), 'stateless direct callback leaf: '+name)
            # Whole linked closure: allocated code/literals/helpers are FLASH,
            # ordinary initialized/zeroed/TLS/static storage is absent.
            import importlib.util
            spec = importlib.util.spec_from_file_location('layout', ROOT/'tests/boot/test_layout.py')
            layout = importlib.util.module_from_spec(spec); spec.loader.exec_module(layout)
            sections, _, loads = layout.elf_tables(elf.read_bytes())
            allowed = {'.boot_continuity': (0x2000dbc0,1024),
                       '.boot_diagnostic': (0x2000dfc0,64), '.stack': (0x2000e000,8192)}
            for name, h in sections.items():
                if not (h[2] & 2) or h[5] == 0:
                    continue
                if name in allowed:
                    require(h[1] == 8 and (h[3],h[5]) == allowed[name], 'exact NOLOAD reservation '+name)
                else:
                    require(not (h[2]&1) and h[3]+h[5] <= 0x8000,
                            'transitive allocated code/literals must be read-only boot FLASH: '+name)
            require(symbols.get('_continuity_start') == 0x2000dbc0 and
                    symbols.get('_continuity_end') == 0x2000dfc0 and
                    symbols.get('_diagnostic_end') == 0x2000e000, 'combined 1024/64 ABI')
            require(any('__aeabi_uldivmod' in name or '__udivmoddi4' in name for name in symbols),
                    'runtime 64-bit sample division closure actually linked')
            # Keep full disassembly, relocation and map evidence for callback audit.
            run([objdump,'-dr',elf], raw=True)
        def clock():
            if 'clock' not in prepared:
                run(['cmake','-S',ROOT/'tests/ethernet/foreground','-B',out/'clock-build',
                     '-DCMAKE_C_COMPILER='+str(CC),'-DCMAKE_BUILD_TYPE=Debug'])
                run(['cmake','--build',out/'clock-build','--parallel','2','--target','foreground_clock'])
                prepared.add('clock')
        for case in roster:
            if stopped:
                emit({'case': case, 'status': 'invalid', 'reason': 'not attempted after infrastructure failure: '+stopped[:160]}); continue
            try:
                if case in CASES[:3]:
                    host(); run([out/'host-build/test_producer', case], binary=True)
                elif case == 'producer-arm-closure':
                    arm()
                elif case == 'retained-startup':
                    host(); run([out/'host-build/test_startup'], binary=True)
                else:
                    clock(); body = run([out/'clock-build/foreground_clock','systick_offset_no_retained_io'], binary=True)
                    require('FOREGROUND_CASE_OK systick_offset_no_retained_io' in body, 'clock case completion marker')
                emit({'case': case, 'status': 'pass'})
            except Failure as exc:
                emit({'case': case, 'status': 'fail', 'at': exc.at, 'assert': exc.assertion[:240]})
            except Exception as exc:
                stopped = str(exc)
                emit({'case': case, 'status': 'invalid', 'reason': stopped[:240]})
        after = identity()
        metadata.update(inputs_after=after, inputs_unchanged=(before == after), elapsed_seconds=time.monotonic()-started, complete=not stopped, planned=len(roster), observed=len([r for r in records if r.get('case') in roster]))
        if before != after:
            # Preserve observed failures; identity invalidity is a separate execution precondition.
            emit({'case': 'input-identity', 'status': 'invalid', 'reason': 'scoped inputs changed during execution'})
        counts = {s: sum(r.get('status') == s for r in records) for s in ('pass', 'fail', 'invalid')}
        metadata['counts'] = counts
        (out/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
        emit({'summary': {'target': 'timer2-producer', 'kind': 'python', **counts, 'report': str(reportpath), 'log': str(logpath)}})
        emit({'rollup': {**counts, 'targets': 1}})
    for old in sorted(base.glob('*-timer2-producer'))[:-5]:
        if old.is_dir() and not old.is_symlink():
            shutil.rmtree(old)
    if a.human:
        with logpath.open() as source:
            shutil.copyfileobj(source, os.sys.stdout)
        print(reportpath.read_text(), end='')
    return int(bool(counts['fail'] or counts['invalid']))

if __name__ == '__main__':
    raise SystemExit(main())
