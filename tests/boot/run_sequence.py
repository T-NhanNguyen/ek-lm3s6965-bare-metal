#!/usr/bin/env python3
"""FIRST shared production sequence gate (no QEMU/hardware). JSONL stdout; full evidence in private runs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.boot_fingerprints import source_fingerprints

GCC = Path('/Users/nhan/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc')
BIN = GCC.parent
CC = Path('/usr/bin/clang')
CASES = ('signed-run', 'early-stop', 'producer-stop', 'capture-read-failure',
         'capture-fullword-mismatch', 'fresh-clear', 'zero-supported', 'clear-faults',
         'guards', 'structural', 'signature-reject', 'arm-closure')

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
    out = base / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '-shared-sequence')
    out.mkdir(parents=True)
    logpath, reportpath = out / 'run.log', out / 'report.jsonl'
    deadline, started = time.monotonic() + a.budget_seconds, time.monotonic()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='halt_on_error=1:detect_stack_use_after_return=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    records, prepared, stopped = [], set(), None
    # Freeze only shared policy/dependency/fixture/harness inputs and official tools.
    def identity():
        return source_fingerprints(ROOT, profile='sequence')
    tools = [GCC, BIN/'arm-none-eabi-nm', BIN/'arm-none-eabi-objdump', CC]
    for name in ('cmake','python3','make','ar','ranlib','ld'):
        found=shutil.which(name)
        if found: tools.append(Path(found))
    def tool_identity():
        # Include compiler bundle executables and libraries used by Arm link.
        scope=set(tools)
        prefix=BIN.parent
        for directory in (prefix/'libexec',prefix/'bin',prefix/'arm-none-eabi/lib/thumb/v7-m/nofp'):
            if directory.exists():
                scope.update(f for f in directory.rglob('*') if f.is_file())
        for pattern in ('lib/gcc/arm-none-eabi/*/thumb/v7-m/nofp/*','arm-none-eabi/lib/*.specs'):
            scope.update(f for f in prefix.glob(pattern) if f.is_file())
        return {str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(scope) if f.is_file()}
    before, tools_before = {}, {}
    try:
        before, tools_before = identity(), tool_identity()
    except Exception as exc:
        stopped='input/tool freeze failed: '+str(exc)
    metadata = {'roster': roster, 'inputs_before': before, 'tools_before':tools_before,
                'nested_roster': {'clear-faults':['clear-read-failure','effected-clear-write-failure'],
                                  'structural':['null-output','sequence-output-alias','misaligned-output','zero-hz','overflow-hz','null-continuity','null-reader','output-context-alias','diagnostic-output-alias']},
                'configuration':'HOST Release ASan+UBSan live assertions; Arm Cortex-M3 Thumb soft -Os enum32 compile/link only'}
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
                raise Failure('tests/boot/run_sequence.py:0', assertion)
        def oracle(ok, reason):
            if not ok: raise Invalid('oracle selfcheck: '+reason)
        def host():
            if 'host' not in prepared:
                run(['cmake', '-S', ROOT/'tests/boot', '-B', out/'host-build',
                     '-DCMAKE_C_COMPILER='+str(CC), '-DCMAKE_BUILD_TYPE=Release', '-DBOOT_SANITIZERS=ON'])
                targets = ['test_sequence']
                run(['cmake','--build',out/'host-build','--parallel','2','--target',*targets])
                prepared.add('host')
        def arm():
            if not all(t.is_file() for t in (GCC,BIN/'arm-none-eabi-objdump',BIN/'arm-none-eabi-nm')):
                raise Invalid('official GNU Arm 15.3.rel1 bundle missing; no PATH fallback')
            version=run([GCC,'--version'])
            require('15.3' in version, 'official compiler version 15.3')
            # Exact linewise parser selfchecks, including operandless instructions.
            def decode(text):
                rows=[]
                for line in text.splitlines():
                    m=re.fullmatch(r'[ \t]*([0-9a-f]+):[ \t]+(?:[0-9a-f]{4}[ \t]+)+([a-z][a-z0-9.]*)[ \t]*([^\r\n]*)',line)
                    if m: rows.append((int(m[1],16),m[2],m[3]))
                return rows
            oracle(decode(' 0:\tbf00 \tnop\n 2:\tbf00 \tnop\n 4:\tbf00 \tnop\n 6:\t4770 \tbx\tlr\n') ==
                    [(0,'nop',''),(2,'nop',''),(4,'nop',''),(6,'bx','lr')], 'linewise decoder exact NOP/bx tuples')
            oracle(decode(' 8:\tf3ef 8210 \tmrs\tr2, PRIMASK\n') == [(8,'mrs','r2, PRIMASK')], '32-bit instruction oracle exact tuple')
            def symbols(text):
                rows=[]
                for line in text.splitlines():
                    if not line.strip(): continue
                    m=re.fullmatch(r'([0-9a-fA-F]+) ([A-Za-z]) (\S+)',line)
                    if not m: raise Invalid('unparsed nm row: '+line[:120])
                    rows.append((m[3],m[2],int(m[1],16)))
                return rows
            oracle(symbols('00000010 T boot_sequence_run\n00000020 R root\n') ==
                    [('boot_sequence_run','T',16),('root','R',32)], 'nm oracle exact type/address/name tuples')
            build=out/'arm-build'
            run(['cmake','-S',ROOT/'tests/boot','-B',build,
                 '-DCMAKE_SYSTEM_NAME=Generic','-DCMAKE_SYSTEM_PROCESSOR=arm',
                 '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                 '-DCMAKE_C_COMPILER='+str(GCC),'-DCMAKE_BUILD_TYPE=Release','-DBOOT_SANITIZERS=OFF'])
            run(['cmake','--build',build,'--parallel','2','--target','boot_crypto'])
            flags=[GCC,'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-fno-short-enums',
                   '-std=c11','-Os','-ffreestanding','-fno-common','-ffunction-sections','-fdata-sections',
                   '-Wall','-Wextra','-Werror','-I',ROOT/'include','-I',ROOT/'config',
                   '-I',ROOT/'third_party/mbedtls/include','-DMBEDTLS_CONFIG_FILE="mbedtls_boot.h"']
            sources=('sequence','startup','startup_gptm','startup_producer','watchdog_target','watchdog',
                     'diag_publication','diag','runtime_watchdog','runtime','reset_handler','reset',
                     'launch','select','verify','store','format','lifecycle','handoff')
            objects=[]
            for name in sources:
                obj=out/('arm-'+name+'.o')
                run([*flags,'-c',ROOT/('src/boot_'+name+'.c'),'-o',obj]); objects.append(obj)
            elf=out/'sequence-unit.elf'
            # Unit closure deliberately NOT the bootloader layout/32KiB fit.
            # nosys supplies inert libc syscall stubs, never executed.
            run([GCC,'-mcpu=cortex-m3','-mthumb','-mfloat-abi=soft','-nostartfiles',
                 '--specs=nosys.specs','-Wl,--gc-sections,-e,boot_sequence_run,-Ttext=0x0',
                 '-Wl,-Map='+str(out/'sequence-unit.map'),*objects,
                 '-Wl,--start-group',build/'libboot_crypto.a','-lc','-lgcc','-lnosys','-Wl,--end-group','-o',elf])
            require(not run([BIN/'arm-none-eabi-nm','-u',elf]).strip(),'unit final link has no undefined symbols')
            table=symbols(run([BIN/'arm-none-eabi-nm','-n','--defined-only',elf]))
            for name in ('boot_sequence_run','boot_startup_producer_begin_io','boot_runtime_watchdog_run',
                         'boot_reset_handler_run','boot_select','boot_verify_manifest','mbedtls_ecdsa_verify'):
                require(sum(n==name and t in ('T','t') for n,t,_ in table)==1,'exact code definition: '+name)
            rows=decode(run([BIN/'arm-none-eabi-objdump','-d','--disassemble=boot_sequence_run',elf]))
            require(bool(rows),'shared unit has decoded instructions')
            object_rows=decode(run([BIN/'arm-none-eabi-objdump','-d',objects[0]]))
            require(any(op=='mrs' and re.fullmatch(r'r(?:[0-9]|1[0-2]),[ \t]*PRIMASK',args,re.I)
                        for _,op,args in object_rows), 'compiled ARM admission exact mrs register, PRIMASK tuple')
            run([BIN/'arm-none-eabi-objdump','-dr',elf],raw=True)
        # Parser selfchecks support the cases, not additional representatives.
        def completed(body,case): return body.splitlines()==['SEQUENCE_CASE_OK '+case]
        if not completed('SEQUENCE_CASE_OK signed-run\n','signed-run') or completed('SEQUENCE_CASE_OK wrong\n','signed-run'):
            stopped='completion oracle selfcheck failed'
        if re.search(r'([^\s:]+\.(?:c|h)):(\d+):\s*([^\n]+)', 'sequence_host.c:17: out == expected').groups() != ('sequence_host.c','17','out == expected'):
            stopped='assertion parser selfcheck failed'
        for case in roster:
            if stopped:
                emit({'case': case, 'status': 'invalid', 'reason': 'not attempted after infrastructure failure: '+stopped[:160]}); continue
            try:
                if case == 'arm-closure':
                    arm()
                else:
                    host(); body=run([out/'host-build/test_sequence',case],binary=True)
                    require(completed(body,case), 'exact case completion roster')
                emit({'case': case, 'status': 'pass'})
            except Failure as exc:
                emit({'case': case, 'status': 'fail', 'at': exc.at, 'assert': exc.assertion[:240]})
            except Exception as exc:
                stopped = str(exc)
                emit({'case': case, 'status': 'invalid', 'reason': stopped[:240]})
        try:
            after, tools_after = identity(), tool_identity()
        except Exception as exc:
            after,tools_after={},{}
            stopped='post-run identity unavailable: '+str(exc)
            emit({'case':'input-identity','status':'invalid','reason':stopped[:240]})
        nested=metadata['nested_roster']
        metadata['reconciliation']={'missing':[c for c in roster if not any(r.get('case')==c for r in records)],
            'duplicates':[c for c in roster if sum(r.get('case')==c for r in records)>1],
            'nested':{r['case']:{n:('pass' if r['status']=='pass' else 'incomplete-unlocalized') for n in nested[r['case']]}
                      for r in records if r.get('case') in nested}}
        metadata.update(inputs_after=after, inputs_unchanged=(before == after), tools_after=tools_after, tools_unchanged=(tools_before == tools_after), elapsed_seconds=time.monotonic()-started, complete=not stopped and before==after and tools_before==tools_after, planned=len(roster), observed=len([r for r in records if r.get('case') in roster]))
        if before != after or tools_before != tools_after:
            # Preserve observed failures; identity invalidity is a separate execution precondition.
            emit({'case': 'input-identity', 'status': 'invalid', 'reason': 'scoped inputs or tool bytes changed during execution'})
        counts = {s: sum(r.get('status') == s for r in records) for s in ('pass', 'fail', 'invalid')}
        metadata['counts'] = counts
        (out/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
        emit({'summary': {'target': 'shared-sequence', 'kind': 'python', **counts, 'report': str(reportpath), 'log': str(logpath)}})
        emit({'rollup': {**counts, 'targets': 1}})
    for old in sorted(base.glob('*-shared-sequence'))[:-5]:
        if old.is_dir() and not old.is_symlink():
            shutil.rmtree(old)
    if a.human:
        with logpath.open() as source:
            shutil.copyfileobj(source, os.sys.stdout)
        print(reportpath.read_text(), end='')
    return int(bool(counts['fail'] or counts['invalid']))

if __name__ == '__main__':
    raise SystemExit(main())
