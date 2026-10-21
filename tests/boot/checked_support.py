"""Offline checked-handoff runner infrastructure; explicit guards survive -O.

No work on import. No downloads, signing imports, private-key reads or execution
of target code. Synthetic ELF stimuli are never executable-closure evidence.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
GCC = Path('/Users/nhan/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc')
BIN = GCC.parent
CC = Path('/usr/bin/clang')
HOST_CASES = ('launch-authentic-prepare', 'launch-admission-cross-api',
              'target-wdt-pending-exempt', 'target-other-pending-reject', 'target-w1c-irq43',
              'target-hib-gate-failclosed', 'target-io-failure-poison', 'target-terminal-reentry-poison')
DISPATCH = ('boot_sequence_launch', 'boot_runtime_watchdog_launch', 'boot_handoff_target_init',
            'boot_handoff_target_observe', 'boot_handoff_target_operation',
            'boot_handoff_target_terminal_intent', 'boot_startup_prepare', 'boot_startup_begin_dispatch',
            'boot_startup_dispatch', 'boot_startup_terminal', 'boot_startup_stop')
COMMON = ('calloc', 'free', '_sbrk', 'boot_verify_stored')
FORBIDDEN = ('boot_sequence_run', 'Application_Reset_Handler', 'Application_Entry_Witness', 'boot_startup_running')
PRODUCTION_IDENTITY = {
    'CMakeLists.txt':'b816ad94a9d43e9a7b16197d8d4f8d72cab5ebd28310f2fea0c2545faafa4e88',
    'docs/boot-reset-image.md':'2a27474e2f8c2b88a38d0a2897ebffa9ec876f3be5c62e7415f020689681ae0f',
    'include/lm3s6965/boot_sequence.h':'4d7fb61c449d2f1de6d008558c7d0b1118400430bba83ddff56031f99314f8f1',
    'scripts/check-boot-reset-image.py':'6a67fea59e18f7b171922f2d1225d50c07a12d4565a5545b15d94a34e3ae2f2e',
    'src/CMakeLists.txt':'03c566c46d815393ff0daaeeb8c6f71da18a0e8b3f74287bc212434e3e313cbb',
    'src/boot_entry/boot_decision_entry.c':'044a0830950cc23f69b68bf7876608fd943062c5283e2eb0e440263dc86b051a',
    'src/boot_sequence.c':'b73ec8bb037a2291ba6af2d8535954a9ea6485940a8d29ce651016dbe9f078bd',
    'src/boot_runtime_watchdog.c':'8c265daff0a32f83a497943303621a0ba835d2f202307501e77c6664d55018cd',
    'include/lm3s6965/boot_handoff_target.h':'5d9d225e4c67f51c2f1546d93e2bad766c7ad0b0f4b3f9a3b8509296901eb9db',
    'src/boot_handoff_target.c':'259c5795d4a3f22385467e2a110f77b2a64836cbf8ac877167322af9fffc72ec',
}
FIXTURES = ('sequence_host.c', 'sequence_checked.h', 'flash_model.c', 'flash_model.h', 'verify_fixtures.h',
            'handoff_terminal.S', 'handoff_fixture.c', 'handoff_fixture.S', 'handoff_fixture.ld',
            'startup_dispatch_fixture.S', 'startup_app_fixture.c', 'startup_boot_fixture.c',
            'layout_fixture.S', 'producer_arm.c')
NESTED = {
    'launch-authentic-prepare': ['signed-A', 'one-transaction', 'full-intent', 'W1C-write-readback-barriers', 'EMPTY-PREPARED-DISPATCHING'],
    'launch-admission-cross-api': ['descriptor-tail', 'handoff-observation', 'observation-continuity', 'output-tail-diagnostic', 'context-descriptor', 'unmasked', 'callback-BUSY-both', 'snapshot', 'CONSUMED-both', 'decision-first'],
    'target-wdt-pending-exempt': ['IRQ18-disabled-inactive', 'WDT-pending-capture', 'capture-preclear-exemption', 'NVIC-ICPR-clear', 'strict-zero-after-clear'],
    'target-other-pending-reject': ['non-WDT-pending-capture', 'no-writes', 'no-exclude-barriers', 'first-operation-only', 'no-terminal', 'BOOT-poison'],
    'target-w1c-irq43': ['model-W1C-WO-oracle', 'IRQ43-capture', 'no-destructive-effects'],
    'target-hib-gate-failclosed': ['RCGC0-read', 'no-HIB-access', 'BOOT-poison', 'no-later-effects'],
    'target-io-failure-poison': ['effected-VTOR', 'no-next-event', 'BOOT-poison', 'no-later-effects'],
    'target-terminal-reentry-poison': ['ignored-reentry', 'empty-suffix', 'APPLICATION-poison', 'no-later-effects'],
    'dispatch-image-isolation': ['default', 'decision', 'dispatch', 'both-on', 'physical-off', 'typed-linked-closure', 'entry-provenance', 'checkpoints', 'retained-callback-closure', 'terminal-tail', 'app-static-exclusion'],
    'dispatch-checker-closure': ['valid', *['missing:'+n for n in DISPATCH], 'out-of-boot:boot_handoff_target_operation', *['forbidden:'+n for n in FORBIDDEN], 'decision-without-flag', 'decision-with-dispatch']}

class Invalid(Exception):
    pass

class Failure(Exception):
    def __init__(self, assertion, at='tests/boot/checked_support.py:0'):
        self.at, self.assertion = at, assertion
        super().__init__(assertion)

def require(ok, message):
    if not ok:
        frame=inspect.currentframe().f_back
        raise Failure(message, str(Path(frame.f_code.co_filename).relative_to(ROOT))+':'+str(frame.f_lineno))

_ORACLE_EVENTS=None

def oracle(ok, message):
    if _ORACLE_EVENTS is not None:
        frame=inspect.currentframe().f_back
        _ORACLE_EVENTS.append({'assertion':message,'matched':bool(ok),
            'at':str(Path(frame.f_code.co_filename).relative_to(ROOT))+':'+str(frame.f_lineno)})
    if not ok:
        raise Invalid('oracle selfcheck: '+message)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def completed(text, case):
    return text == 'SEQUENCE_CASE_OK '+case+'\n'

def assertion(text):
    m = re.search(r'([^\s:]+\.(?:c|h)):(\d+):\s*([^\n]+)', text)
    return (m[1], int(m[2]), m[3]) if m else None

def nm_rows(text):
    rows=[]
    for line in text.splitlines():
        if not line.strip(): continue
        m=re.fullmatch(r'([0-9a-fA-F]+) ([A-Za-z]) (\S+)', line)
        if not m: raise Invalid('unparsed nm row: '+line[:120])
        rows.append((m[3],m[2],int(m[1],16)))
    return rows

def instruction_rows(text, symbol):
    # Existing strict parser; comments/case normalization are explicit in it.
    from test_handoff_target import instructions
    try:
        return instructions(text,symbol)
    except RuntimeError as exc:
        raise Invalid(str(exc)) from exc

class Elf:
    """Independent typed ELF32 reader; preserve section membership and size."""
    def __init__(self, data):
        if data[:7] != b'\x7fELF\x01\x01\x01': raise Invalid('ELF32 LSB required')
        h=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
        if h[2]!=40: raise Invalid('ARM ELF required')
        self.header=('ELF32','LSB','ARM',h[4]); self.data=data
        self.sections={}; self.symbols={}; self.bindings={}; self.loads=[]
        table=[struct.unpack_from('<10I',data,h[6]+i*h[11]) for i in range(h[12])]
        def string(blob,off):
            return blob[off:blob.index(b'\0',off)].decode('ascii')
        ns=table[h[13]]; names=data[ns[4]:ns[4]+ns[5]]
        indexed={i:string(names,s[0]) for i,s in enumerate(table)}
        for i,s in enumerate(table):
            name=indexed[i]
            if name in self.sections: raise Invalid('duplicate ELF section '+name)
            self.sections[name]=s
            if s[1]!=2: continue
            st=table[s[6]]; strings=data[st[4]:st[4]+st[5]]
            if s[9]!=16: raise Invalid('unexpected ELF symbol stride')
            for off in range(s[4],s[4]+s[5],16):
                no,value,size,info,other,ndx=struct.unpack_from('<IIIBBH',data,off)
                name=string(strings,no)
                if not name: continue
                row=(name,info&15,('UNDEF' if ndx==0 else indexed.get(ndx,'ABS' if ndx==0xfff1 else 'UNDEF')),value,size)
                self.symbols.setdefault(name,[]).append(row)
                self.bindings.setdefault(name,[]).append(info>>4)
        for i in range(h[10]):
            p=struct.unpack_from('<8I',data,h[5]+i*h[9])
            if p[0]==1: self.loads.append(p)
    def symbol(self,name):
        rows=self.symbols.get(name,[])
        bindings=self.bindings.get(name,[])
        # Global authority wins over same-name translation-unit locals. Local
        # scratch remains available only when unique; never collapse records.
        if 1 in bindings: rows=[r for r,b in zip(rows,bindings) if b==1]
        else: rows=[r for r,b in zip(rows,bindings) if b==0]
        if len(rows)!=1: raise Invalid('missing/ambiguous ELF symbol '+name)
        return rows[0]
    def value(self,name): return self.symbol(name)[3]
    def code(self,name,address=None):
        rows=self.symbols.get(name,[])
        if address is not None: rows=[r for r in rows if (r[3]&~1)==address]
        else:
            bindings=self.bindings.get(name,[])
            rows=[r for r,b in zip(rows,bindings) if b==1]
        if len(rows)>1 and address is None: raise Invalid('ambiguous required executable ELF anchor '+name)
        if len(rows)!=1: return False
        _,kind,section,value,size=rows[0]
        s=self.sections.get(section)
        return bool(kind==2 and s and s[2]&4 and s[1]==1 and size>0 and
                    0<(value&~1)<0x8000 and s[3]<=value&~1 and (value&~1)+size<=s[3]+s[5])

def selfchecks():
    case='launch-authentic-prepare'; good='SEQUENCE_CASE_OK '+case+'\n'
    oracle(completed(good,case),'completion')
    for bad in (good[:-1],good+good,good+'extra\n','',good.replace(case,'wrong')):
        oracle(not completed(bad,case),'reject incomplete/extra/duplicate completion')
    oracle(assertion('sequence_host.c:17: out == expected')==('sequence_host.c',17,'out == expected'),'assertion tuple')
    sample='00000000 <sample>:\n 0: bf00 nop\n 2: f3ef 8210 mrs r2, PRIMASK\n 6: 4770 bx lr\n'
    oracle(instruction_rows(sample,'sample')==[(0,'nop',''),(2,'mrs','r2, primask'),(6,'bx','lr')],'instruction tuples lowercase operands')
    for text,name in ((sample,'missing'),(sample+' 8: MALFORMED\n','sample')):
        try: instruction_rows(text,name)
        except Invalid: pass
        else: raise Invalid('instruction parser admitted malformed/missing block')
    oracle(nm_rows('00000010 T boot_sequence_launch\n00000020 R root\n')==[('boot_sequence_launch','T',16),('root','R',32)],'nm typed tuples')
    from test_boot_reset_image import fixture
    elf=Elf(fixture())
    oracle(elf.header==('ELF32','LSB','ARM',0x101),'independent ELF header')
    oracle((elf.sections['.isr_vector'][1],elf.sections['.isr_vector'][3],elf.sections['.isr_vector'][5])==(1,0,236),'vector section')
    oracle((elf.sections['.text'][1],elf.sections['.text'][3],elf.sections['.text'][5])==(1,0x100,4),'text section')
    oracle((elf.loads[0][0],elf.loads[0][3],elf.loads[0][4])==(1,0,236),'physical LOAD')
    words=struct.unpack_from('<59I',elf.data,elf.sections['.isr_vector'][4])
    oracle(words[:2]==(0x20010000,0x101) and all(words[i]==0 for i in (7,8,9,10,13)),'vector words')
    oracle(elf.symbol('boot_sequence_run')==('boot_sequence_run',0,'UNDEF',0x10d,0),'synthetic UNDEF NOTYPE tuple')
    oracle(not elf.code('boot_sequence_run'),'UNDEF is never executable closure')
    # Independent captured typed/object/undefined symbol format stimuli.
    raw=bytearray(fixture())
    h=struct.unpack_from('<16sHHIIIIIHHHHHH',raw)
    sym=struct.unpack_from('<10I',raw,h[6]+5*40)
    struct.pack_into('<I',raw,sym[4]+16+8,4)
    struct.pack_into('<BBH',raw,sym[4]+16+12,0x12,0,3)
    # Mark .text executable; do not use the checker as oracle for this model.
    struct.pack_into('<I',raw,h[6]+3*40+8,6)
    typed=Elf(raw)
    oracle(typed.symbol('Boot_Reset_Handler')==('Boot_Reset_Handler',2,'.text',0x101,4),'typed section tuple')
    oracle(typed.code('Boot_Reset_Handler'),'defined executable function')
    raw[sym[4]+16+12]=0x11
    oracle(not Elf(raw).code('Boot_Reset_Handler'),'lookalike object is not function')

def source_identity():
    # Test-local expansion of existing sequence profile; no production helper edit.
    sys.path.insert(0,str(ROOT))
    from scripts.boot_fingerprints import source_fingerprints
    result=source_fingerprints(ROOT,profile='sequence')
    paths=[ROOT/'CMakeLists.txt',ROOT/'scripts/check-boot-reset-image.py',ROOT/'docs/boot-reset-image.md']
    for directory in ('tests/boot','cmake','linker','src/boot_entry'):
        paths.extend(p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ('.py','.c','.h','.S','.ld','.cmake','.json','.txt'))
    paths.extend((ROOT/'src').glob('boot_*.S'))
    for p in paths:
        if p.is_symlink(): raise Invalid('source alias '+str(p))
        result[str(p.relative_to(ROOT))]=sha(p)
    return dict(sorted(result.items()))

def tool_identity():
    tools=[GCC,CC,*(BIN/('arm-none-eabi-'+n) for n in ('nm','objdump','readelf','objcopy','size'))]
    for name in ('cmake','python3','make','ar','ranlib','ld'):
        path=shutil.which(name)
        if not path: raise Invalid('missing tool '+name)
        tools.append(Path(path))
    if any(not p.is_file() for p in tools): raise Invalid('pinned official Arm/host tools missing; no fallback')
    scope=set(tools)
    for directory in (BIN.parent/'libexec',BIN.parent/'arm-none-eabi/lib/thumb/v7-m/nofp'):
        if not directory.is_dir(): raise Invalid('missing official runtime bundle '+str(directory))
        scope.update(p for p in directory.rglob('*') if p.is_file())
    for pattern in ('lib/gcc/arm-none-eabi/*/thumb/v7-m/nofp/*','arm-none-eabi/lib/*.specs'):
        scope.update(p for p in BIN.parent.glob(pattern) if p.is_file())
    return {str(p):sha(p) for p in sorted(scope)}

def cache_root():
    value=os.environ.get('CHECKED_HANDOFF_BUILD_ROOT')
    if not value: raise Invalid('explicit CHECKED_HANDOFF_BUILD_ROOT required')
    path=Path(value)
    if not path.is_absolute() or path.is_symlink(): raise Invalid('cache must be literal absolute non-symlink path')
    path=path.resolve()
    if not path.is_relative_to(Path('/tmp').resolve()) or path==Path('/tmp').resolve():
        raise Invalid('cache must be unique directory below /tmp')
    return path

def arguments(cases, support=None, extra=None):
    p=argparse.ArgumentParser()
    group=p.add_mutually_exclusive_group(required=True)
    group.add_argument('--case',action='append',choices=cases)
    group.add_argument('--all',action='store_true')
    if support: group.add_argument('--'+support,action='store_true')
    p.add_argument('--budget-seconds',type=int,required=True,choices=range(1,601))
    p.add_argument('--human',action='store_true'); p.add_argument('--failures-only',action='store_true')
    if extra: extra(p)
    a=p.parse_args()
    if a.case and len(a.case)!=len(set(a.case)): p.error('duplicate case')
    return a,[c for c in cases if a.all or c in (a.case or [])]

class Session:
    def __init__(self,target,args,roster):
        self.args=args; self.target=target; self.roster=roster; self.records=[]
        self.started=time.monotonic(); self.deadline=self.started+args.budget_seconds
        base=ROOT/'.dev-vault/tests/runs'
        self.out=base/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-'+target)
        self.out.mkdir(parents=True)
        self.logpath=self.out/'run.log'; self.reportpath=self.out/'report.jsonl'
        self.log=self.logpath.open('wb'); self.report=self.reportpath.open('w')
        self.manifest={'roster':roster,'nested_roster':{c:NESTED[c] for c in roster},'nested_observed':{},
                       'configuration':target,'support_actions':[], 'limits':'HOST model/static ARM only; no application execution, dynamic peaks or physical timing'}
        self.env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',ASAN_OPTIONS='halt_on_error=1:detect_stack_use_after_return=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        self.env.pop('PYTHONOPTIMIZE',None)
    def run(self,argv,expected=0):
        argv=list(map(str,argv)); remaining=self.deadline-time.monotonic()
        if remaining<=0: raise Invalid('allocated deadline exhausted')
        self.log.write(('\n$ '+repr(argv)+'\n').encode()); self.log.flush(); start=self.log.tell()
        child=None
        try:
            child=subprocess.Popen(argv,cwd=ROOT,env=self.env,stdout=self.log,stderr=subprocess.STDOUT,start_new_session=True)
            child.wait(timeout=remaining)
        except (OSError,subprocess.SubprocessError) as exc:
            if child is not None:
                try: os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                child.wait()
            raise Invalid(str(exc)) from exc
        self.log.flush(); end=self.log.tell()
        with self.logpath.open('rb') as f:
            f.seek(start); raw=f.read(min(end-start,4*1024*1024))
        if end-start>4*1024*1024: raise Invalid('parser evidence >4MiB; full raw log retained')
        body=raw.decode('utf-8',errors='replace')
        if expected is not None and child.returncode!=expected:
            raise Invalid('child exit '+str(child.returncode)+'; raw command/output in run.log')
        return child.returncode,body
    def freeze(self,fresh=False,migration=None):
        self.root=cache_root()
        self.identity={'sources':source_identity(),'tools':tool_identity()}
        for path,digest in PRODUCTION_IDENTITY.items():
            if self.identity['sources'].get(path)!=digest:
                raise Invalid('approved production overlay changed: '+path+'; renewed adjudication required')
        self.manifest.update(cache=str(self.root),identity=self.identity)
        file=self.root/'checked-identity.json'
        if fresh:
            if self.root.exists() and any(self.root.iterdir()): raise Invalid('bringup root is not fresh')
            self.root.mkdir(parents=True,exist_ok=True)
            file.write_text(json.dumps(self.identity,sort_keys=True,indent=2)+'\n')
        elif migration is not None:
            # Migration validates the OLD immutable cache before source equality.
            # Never rewrite checked-identity.json to manufacture compatibility.
            migration(self,file)
        elif not file.is_file() or json.loads(file.read_text())!=self.identity:
            raise Invalid('cache identity absent/mismatched; no implicit rebuild')
    def emit(self,record):
        self.records.append(record); line=json.dumps(record,separators=(',',':'))
        self.report.write(line+'\n'); self.report.flush()
        if not self.args.human and (not self.args.failures_only or record.get('status')!='pass'):
            print(line,flush=True)
    def nested(self,case,name):
        seen=self.manifest['nested_observed'].setdefault(case,[])
        if name not in NESTED[case] or name in seen: raise Invalid('unexpected/duplicate nested record '+name)
        seen.append(name)
    def execute(self,callback,setup):
        stopped=None
        try: setup()
        except Exception as exc: stopped=str(exc)
        for case in self.roster:
            if stopped:
                self.emit({'case':case,'status':'invalid','reason':'not attempted: '+stopped[:200]}); continue
            try:
                callback(case)
                observed=self.manifest['nested_observed'].get(case,[])
                if sorted(observed)!=sorted(NESTED[case]): raise Invalid('incomplete nested roster')
                self.emit({'case':case,'status':'pass'})
            except Failure as exc:
                self.emit({'case':case,'status':'fail','at':exc.at,'assert':exc.assertion[:240]})
            except Exception as exc:
                stopped=str(exc); self.emit({'case':case,'status':'invalid','reason':stopped[:240]})
        if not self.roster:
            self.manifest['support_actions'].append({'status':'invalid' if stopped else 'complete','reason':stopped})
        try:
            if hasattr(self,'identity') and self.identity!={'sources':source_identity(),'tools':tool_identity()}:
                raise Invalid('source/tool bytes changed during invocation')
        except Exception as exc:
            self.emit({'case':'input-identity','status':'invalid','reason':str(exc)[:240]})
        if not self.roster and stopped:
            self.emit({'case':'support-precondition','status':'invalid','reason':stopped[:240]})
        if time.monotonic()>self.deadline:
            self.emit({'case':'execution-budget','status':'invalid','reason':'allocated deadline exhausted including identity/output handling'})
        counts={s:sum(r.get('status')==s for r in self.records) for s in ('pass','fail','invalid')}
        reconciliation={c:{'planned':NESTED[c], 'observed':self.manifest['nested_observed'].get(c,[]),
            'missing':[n for n in NESTED[c] if n not in self.manifest['nested_observed'].get(c,[])]} for c in self.roster}
        self.manifest.update(counts=counts,elapsed_seconds=time.monotonic()-self.started,reconciliation=reconciliation,
            complete=not stopped and not counts['invalid'] and all(not r['missing'] for r in reconciliation.values()),
            planned=len(self.roster),observed=sum(r.get('case') in self.roster for r in self.records))
        (self.out/'manifest.json').write_text(json.dumps(self.manifest,indent=2)+'\n')
        self.emit({'summary':{'target':self.target,'kind':'python',**counts,'report':str(self.reportpath),'log':str(self.logpath)}})
        self.emit({'rollup':{**counts,'targets':1}})
        self.log.close(); self.report.close()
        for old in sorted(self.out.parent.glob('*-'+self.target))[:-5]:
            if old.is_dir() and not old.is_symlink() and not (old/'.preserve').exists(): shutil.rmtree(old)
        if self.args.human:
            print(self.logpath.read_text(),end=''); print(self.reportpath.read_text(),end='')
        return int(bool(counts['fail'] or counts['invalid']))


HANDOFF_CASES=('entry-inline-handoff','retained-callback-closure',
               'terminal-transition','ftpa-initializer-native-abi')
HARNESS_PATHS=tuple('tests/boot/'+n for n in
    ('checked_entry.py','checked_static.py','checked_support.py','run_checked_dispatch.py'))
ORIGINAL_MANIFEST_SHA='51977f3e7ab313c9781ed3a7ad0b8eb11bb3681caa6b7ba96ad68e90dfb194e7'
PRODUCTION_SHA='60c31398881c87ef04cff19ed971f0e5ef0a5cd8b0aa8f5f4e193f32c3309d36'
FTPA_SHA='28c816ca722a51caee9b42ab5c4929e041e0b3b25c76b376976c811d72ee6842'
# All required proof obligations are now authored (NOT RUN). Gaps remain
# empty to force INVALID if a future roster reintroduces a missing control.
AUTHORING_GAPS={
    'entry-inline-handoff': (),
    'retained-callback-closure': (),
    'terminal-transition': (),
    'ftpa-initializer-native-abi': (),
}

# Literal bindings, never inferred from an external roster. A gate's returned
# semantic evidence is required; a file digest alone cannot satisfy a binding.
HANDOFF_BINDINGS={c:{} for c in HANDOFF_CASES}
def _bind(case, gate, field, names):
    for name in names.split():
        if name in HANDOFF_BINDINGS[case]: raise RuntimeError('duplicate binding '+name)
        HANDOFF_BINDINGS[case][name]=(gate,field)

_bind(HANDOFF_CASES[0],'dispatch-entry','paths', '''entry-from-first-instruction
cfg-flags-widths-it-aapcs mmio-leaf-body-validation clock-success-transcript
clock-failure-transcript flash-init-failure-transcript first-image-read-failure
second-image-read-failure target-init-reject observe-reject prepare-reject
aggregate-and-launch-original-pointers complete-publication-poison-bytes
stop-terminal-body-order ready-publication-barrier-order descriptor-all40-all48
adapter-and-retained-context-bindings reject-sequence-status reject-sequence-stage
reject-watchdog-wrapper-status reject-missing-runtime-valid reject-missing-handoff-valid
reject-nested-runtime-status reject-handoff-status''')
_bind(HANDOFF_CASES[0],'dispatch-entry','cfg','cfg-no-bypass-dominance-no-retry')
_bind(HANDOFF_CASES[0],'decision-entry','paths','decision-one-run-no-retry')
_bind(HANDOFF_CASES[0],'preservation','resources','preserved-structural-config-resources')
_bind(HANDOFF_CASES[0],'entry_selfchecks','completion', '''stmdb-architectural-order
stmdb-alias-controls stmdb-illegal-controls mutate-unknown-mmio-read mutate-mmio-leaf-semantics
corrupt-interpreter-branch corrupt-interpreter-field-offset corrupt-interpreter-copy
corrupt-interpreter-barrier-recording''')
_bind(HANDOFF_CASES[0],'dispatch-entry','semantic_controls', '''mutate-guard-sequence-status
mutate-guard-sequence-stage mutate-guard-watchdog-wrapper-status mutate-guard-runtime-valid
mutate-guard-handoff-valid mutate-guard-nested-runtime-status mutate-guard-handoff-status
mutate-runtime-pointer mutate-launch-pointer mutate-original-result-pointer
mutate-publication-order mutate-launch-retry mutate-descriptor-binding
mutate-context-binding mutate-publication-byte mutate-stop-dmb mutate-ready11-dmb
mutate-ready13-dmb mutate-unknown-mmio-address''')
_bind(HANDOFF_CASES[1],'callbacks','visited','typed-address-qualified-closure')
_bind(HANDOFF_CASES[1],'callbacks','indirect_bindings','indirect-target-context-fields')
_bind(HANDOFF_CASES[1],'callbacks','native_descriptor_copy','native-descriptor-copy')
_bind(HANDOFF_CASES[1],'callbacks','closure_boundaries','no-ordinary-bss-libc-dependency entry-binding-composite-dependency')
_bind(HANDOFF_CASES[1],'static_selfchecks','completion','mutate-ordinary-ram-borrowing')
_bind(HANDOFF_CASES[2],'terminal','terminal','vtor-dsb-isb-lr-msp-control-masks-bx no-old-stack-after-msp')
_bind(HANDOFF_CASES[2],'terminal','observation_mrs','observation-exclusion-tuples')
_bind(HANDOFF_CASES[2],'terminal','wrapper','dispatch-wrapper-parameter-provenance')
_bind(HANDOFF_CASES[2],'static_selfchecks','completion', '''mutate-terminal-vtor
mutate-terminal-old-stack mutate-terminal-barrier mutate-terminal-parameter''')
_bind(HANDOFF_CASES[3],'ftpa','sha256','historical-fixture-identity')
_bind(HANDOFF_CASES[3],'ftpa','initializer_write_intervals','actual-initializer-loops')
_bind(HANDOFF_CASES[3],'ftpa','machine_contracts', '''pre-c-capture-witness-success-guard
native-abi-magic-version-size native-callback-context-ownership native-entry-transition-register-bindings''')
_bind(HANDOFF_CASES[3],'static_selfchecks','completion','mutate-initializer-overlap mutate-native-abi-offset')


def read_json(path):
    path=Path(path)
    if path.is_symlink() or not path.is_file(): raise Invalid('missing/aliased evidence '+str(path))
    def pairs(items):
        result={}
        for key,value in items:
            if key in result: raise Invalid('duplicate JSON key '+key)
            result[key]=value
        return result
    return json.loads(path.read_text(),object_pairs_hook=pairs)

def immutable_json(path,value):
    path=Path(path)
    if path.is_symlink(): raise Invalid('sidecar alias')
    # Exclusive creation; a repair must supply a distinct path and parent digest.
    with path.open('x') as stream:
        stream.write(json.dumps(value,sort_keys=True,indent=2)+'\n')

def handoff_roster(path):
    roster=read_json(path)
    if roster.get('schema')!='checked-handoff-obligations.v1': raise Invalid('unsupported obligation roster')
    if roster.get('representatives')!=list(HANDOFF_CASES): raise Invalid('literal four-case roster required')
    if roster.get('harness')!={p:sha(ROOT/p) for p in HARNESS_PATHS}:
        raise Invalid('roster does not bind authored harness bytes')
    if roster.get('production_sha256')!=PRODUCTION_SHA:
        raise Invalid('roster production identity')
    if set(roster.get('obligations',{}))!=set(HANDOFF_CASES):
        raise Invalid('unknown/missing representative obligations')
    for case in HANDOFF_CASES:
        obligations=roster.get('obligations',{}).get(case)
        if not isinstance(obligations,list) or not obligations: raise Invalid('missing nested roster '+case)
        ids=[o.get('id') for o in obligations]
        if any(not isinstance(i,str) or not i for i in ids) or len(ids)!=len(set(ids)):
            raise Invalid('missing/duplicate nested obligation '+case)
        if any(o.get('expected') is None or not o.get('bound') for o in obligations):
            raise Invalid('missing handwritten expectation/bound '+case)
        if set(ids)!=set(HANDOFF_BINDINGS[case])|set(AUTHORING_GAPS[case]):
            raise Invalid('exact implemented/missing obligation roster mismatch '+case)
    return roster

def validate_source_migration(old,current,harness):
    if set(harness)!=set(HARNESS_PATHS): raise Invalid('migration harness allowlist')
    if set(old)!=set(current): raise Invalid('migration source additions/omissions')
    for path,digest in old.items():
        if current[path]!=(harness[path] if path in HARNESS_PATHS else digest):
            raise Invalid('unapproved source migration '+path)


def handoff_migration_selfchecks():
    # Independent tiny records; no live manifest or SUT-generated expectation.
    old={p:'old' for p in HARNESS_PATHS}; old['src/frozen.c']='production'
    harness={p:'new' for p in HARNESS_PATHS}
    current={**harness,'src/frozen.c':'production'}
    oracle(validate_source_migration(old,current,harness) is None,'four approved source changes')
    for changed,reason in ((dict(current,**{'src/frozen.c':'changed'}),'unapproved source migration'),
            (dict(current,**{'extra.py':'new'}),'migration source additions/omissions'),
            ({p:h for p,h in current.items() if p!='src/frozen.c'},'migration source additions/omissions'),
            (dict(current,**{HARNESS_PATHS[0]:'unapproved'}),'unapproved source migration')):
        try: validate_source_migration(old,changed,harness)
        except Invalid as exc: oracle(str(exc).startswith(reason),'migration intended rejection')
        else: raise Invalid('migration mutation escaped')
    oracle(old['src/frozen.c']=='production' and old[HARNESS_PATHS[0]]=='old',
           'migration leaves original fixtures immutable')
    # Literal expected sidecar dimensions, independent of a live cache/parser.
    expected={'manifest':'original','cache':'old-cache','tools':['compiler','runtime'],
        'artifacts':['decision','dispatch'],'build':['flags','link','commands'],
        'production':'364','old_harness':'old','new_harness':'new','parent':None}
    validate_migration_record(dict(expected),expected)
    for key in expected:
        for changed in ({**expected,key:'corrupt'}, {k:v for k,v in expected.items() if k!=key}):
            try: validate_migration_record(changed,expected)
            except Invalid as exc: oracle(str(exc)=='migration sidecar exact record mismatch','sidecar intended rejection')
            else: raise Invalid('sidecar mutation escaped '+key)
    try: validate_migration_record({**expected,'unknown':'extra'},expected)
    except Invalid: pass
    else: raise Invalid('sidecar addition escaped')


def validate_migration_record(actual,expected):
    if actual!=expected: raise Invalid('migration sidecar exact record mismatch')


def validate_handoff_migration(s,cache_file,prior_path,roster,sidecar=None,write=None,parent=None):
    """Identity is necessary, never a semantic oracle or permission to rebuild."""
    prior_path=Path(prior_path)
    manifest_path=prior_path/'manifest.json'
    if sha(manifest_path)!=ORIGINAL_MANIFEST_SHA: raise Invalid('original manifest identity')
    old=read_json(manifest_path)
    if len(old['identity']['sources'])!=449: raise Invalid('original source roster size')
    if old['cache']!=str(s.root) or read_json(cache_file)!=old['identity']:
        raise Invalid('original immutable cache identity')
    current=s.identity
    if current['tools']!=old['identity']['tools']: raise Invalid('captured tools/runtime bundle changed')
    sources=old['identity']['sources']
    if set(current['sources'])!=set(sources): raise Invalid('unexplained source additions/omissions')
    validate_source_migration(sources,current['sources'],roster['harness'])
    production={p:h for p,h in current['sources'].items() if not p.startswith('tests/')}
    block=''.join(h+'  '+p+'\n' for p,h in sorted(production.items()))
    if len(production)!=364 or hashlib.sha256(block.encode()).hexdigest()!=PRODUCTION_SHA:
        raise Invalid('364-record production fingerprint')
    artifacts={p:h for group in old['artifacts'].values() for p,h in group.items()}
    if len(artifacts)!=8: raise Invalid('exact eight decision/dispatch artifacts required')
    for path,digest in artifacts.items():
        if sha(path)!=digest: raise Invalid('immutable artifact changed '+path)
    caches={}
    for name,record in old['configurations'].items():
        path=s.root/'arm-public-repair-1'/name/'CMakeCache.txt'
        if sha(path)!=record['cache']: raise Invalid('captured configuration changed '+name)
        caches[str(path)]=record['cache']
    objects={}
    for record in old['entry_objects'].values():
        if sha(record['path'])!=record['sha256']: raise Invalid('entry object changed')
        objects[record['path']]=record['sha256']
        # Locate only within this object's existing build; do not configure.
        candidates=[p for p in Path(record['path']).parents if (p/'compile_commands.json').is_file()]
        if not candidates: raise Invalid('missing original compile commands')
        commands=read_json(candidates[0]/'compile_commands.json')
        entries=[r for r in commands if r['file'].endswith('/boot_entry/boot_decision_entry.c')]
        if len(entries)!=1 or entries[0].get('command',' '.join(entries[0].get('arguments',[])))!=record['compile_command']:
            raise Invalid('captured entry compile command changed')
        caches[str(candidates[0]/'compile_commands.json')]=sha(candidates[0]/'compile_commands.json')
    # Capture nested configuration/build evidence in place, never rebuild it.
    # Original hashes/entry commands are the historical anchors; extra build
    # records below are frozen captures, not retroactive original-build claims.
    build_capture={}
    for name in ('default','decision','dispatch','both-on','physical-off'):
        directory=s.root/'arm-public-repair-1'/name
        if not directory.is_dir(): raise Invalid('missing historical configuration '+name)
        paths=[p for p in directory.rglob('*') if p.is_file() and
               (p.name in ('CMakeCache.txt','compile_commands.json','flags.make','link.txt',
                           'TargetDirectories.txt','CMakeConfigureLog.yaml') or p.suffix=='.cmake')]
        if not paths: raise Invalid('missing build capture '+name)
        for path in paths:
            if path.is_symlink(): raise Invalid('aliased build evidence '+str(path))
            build_capture[str(path)]=sha(path)
    historical={str(p):sha(p) for p in sorted(prior_path.rglob('*')) if p.is_file()}
    if any(p.is_symlink() for p in prior_path.rglob('*')): raise Invalid('aliased historical evidence')
    support=s.manifest.get('oracle_support')
    if sidecar:
        supplied=read_json(sidecar)
        support=supplied.get('oracle_support')
        validate_support_evidence(support,roster['harness'])
        s.manifest['oracle_support']=support
    if not support: raise Invalid('missing evaluated bringup selfcheck evidence')
    validate_support_evidence(support,roster['harness'])
    parent_digest=None; generation=1
    if sidecar and not parent:
        parent=read_json(sidecar).get('parent_path')
    if parent:
        previous=read_json(parent)
        if (previous.get('schema')!='checked-handoff-migration.v1' or
            previous.get('production_sha256')!=PRODUCTION_SHA or
            previous.get('sources')!=sources or previous.get('tools')!=current['tools'] or
            previous.get('artifacts')!=artifacts):
            raise Invalid('untrusted migration parent')
        for key,value in {'manifest_sha256':sha(manifest_path),'cache_identity_sha256':sha(cache_file),
                'build_records':caches,'entry_objects':objects,'build_capture':build_capture,
                'historical_records':historical,'configuration_records':old['configurations']}.items():
            if previous.get(key)!=value: raise Invalid('migration parent changed '+key)
        validate_support_evidence(previous.get('oracle_support'),previous.get('new_harness'))
        generation=previous.get('generation',0)+1
        if generation<2: raise Invalid('invalid repair generation')
        parent_digest=sha(parent)
    destination=Path(write or sidecar) if write or sidecar else None
    if destination is None:
        raise Invalid('exclusive versioned migration filename required')
    if generation==1 and parent is None:
        # First-generation sidecar: no v1..v4 parent chain was ever minted, so the
        # roster-aligned sidecar (e.g. `.v5.json`/`.v6.json`) serves as generation 1.
        # Any explicitly versioned `.v<N>.json` name is accepted; bare/unversioned
        # names are still rejected, preserving exclusive versioning. Content/SHA
        # binding and read/write exclusivity below are unchanged.
        if not re.search(r'\.v\d+\.json$',destination.name):
            raise Invalid('exclusive versioned migration filename required')
    elif not destination.name.endswith('.v'+str(generation)+'.json'):
        raise Invalid('exclusive versioned migration filename required')
    if parent and destination.resolve()==Path(parent).resolve(): raise Invalid('cannot overwrite migration parent')
    expected={'schema':'checked-handoff-migration.v1','generation':generation,
        'obligation_roster_sha256':s.manifest['obligation_roster_sha256'],
        'manifest_sha256':sha(manifest_path),
        'cache_identity_sha256':sha(cache_file),'production_sha256':PRODUCTION_SHA,
        'old_harness':{p:sources[p] for p in HARNESS_PATHS},'new_harness':roster['harness'],
        'sources':sources,'tools':current['tools'],'artifacts':artifacts,'build_records':caches,
        'entry_objects':objects,'build_capture':build_capture,'historical_records':historical,
        'configuration_records':old['configurations'],'oracle_support':support,
        'parent_sha256':parent_digest,'parent_path':str(parent) if parent else None}
    if write:
        if sidecar: raise Invalid('read/write migration are exclusive')
        immutable_json(write,expected)
    elif not sidecar:
        raise Invalid('missing explicit migration sidecar')
    else: validate_migration_record(read_json(sidecar),expected)
    s.manifest['migration']=expected
    s.manifest['historical_manifest']={'path':str(manifest_path),'sha256':ORIGINAL_MANIFEST_SHA,
        'outcomes':old.get('counts'),'reconciliation':old.get('reconciliation')}
    return old

SUPPORT_GATES=('selfchecks','entry_selfchecks','static_selfchecks',
               'handoff_reporting_selfchecks','handoff_migration_selfchecks')


def validate_support_evidence(reference,harness):
    if not isinstance(reference,dict) or set(reference)!={'path','sha256'}:
        raise Invalid('support evidence reference')
    if sha(reference['path'])!=reference['sha256']: raise Invalid('support evidence changed')
    evidence=read_json(reference['path'])
    if evidence.get('schema')!='checked-handoff-support.v1' or evidence.get('harness')!=harness:
        raise Invalid('support evidence harness/schema mismatch')
    records=evidence.get('records',[])
    if [r.get('gate') for r in records]!=list(SUPPORT_GATES): raise Invalid('support exact gate roster')
    for record in records:
        if (record.get('status')!='pass' or record.get('attempted') is not True or
                record.get('evidence',{}).get('completion') is not True or
                not record.get('evidence',{}).get('assertions') or not record.get('at')):
            raise Invalid('support selfcheck incomplete '+str(record.get('gate')))
    return records


class HandoffEvidence:
    """Capture actual evaluated returns/exceptions before nested reconciliation."""
    def __init__(self,s):
        self.s=s; self.gates={}; self.case_gates={c:[] for c in HANDOFF_CASES}
    def evaluate(self,gate,callback,case=None):
        if gate in self.gates: raise Invalid('duplicate gate evidence '+gate)
        at=str(Path(callback.__code__.co_filename).relative_to(ROOT))+':'+str(callback.__code__.co_firstlineno)
        record={'gate':gate,'at':at,'attempted':True}
        global _ORACLE_EVENTS
        previous_events=_ORACLE_EVENTS
        samples=[] if gate in SUPPORT_GATES else None
        _ORACLE_EVENTS=samples
        try:
            value=callback()
            if value is None: value={'completion':True,'assertions':samples or []}
            if not isinstance(value,dict) or not value: raise Invalid('empty/non-object gate evidence '+gate)
            record.update(status='pass',evidence=value)
        except Failure as exc:
            record.update(status='fail',at=exc.at,assertion=exc.assertion)
        except Exception as exc:
            record.update(status='invalid',reason=str(exc))
        finally: _ORACLE_EVENTS=previous_events
        path=self.s.out/(gate+'-evaluated.json')
        immutable_json(path,record)
        record['artifact']={'path':str(path),'sha256':sha(path)}
        self.gates[gate]=record
        if case: self.case_gates[case].append(gate)
        return record
    def support(self,callbacks):
        for gate,callback in callbacks: self.evaluate(gate,callback)
        path=self.s.out/'oracle-support.json'
        immutable_json(path,{'schema':'checked-handoff-support.v1',
            'harness':{p:sha(ROOT/p) for p in HARNESS_PATHS},
            'records':[self.gates[n] for n in SUPPORT_GATES]})
        reference={'path':str(path),'sha256':sha(path)}
        self.s.manifest['oracle_support']=reference
        validate_support_evidence(reference,{p:sha(ROOT/p) for p in HARNESS_PATHS})
        # This newly created run is a dependency of immutable sidecars. Retention
        # must never delete it even after five later ordinary/failed runs.
        (self.s.out/'.preserve').touch(exist_ok=False)
    def restore_support(self):
        records=validate_support_evidence(self.s.manifest['oracle_support'],
            {p:sha(ROOT/p) for p in HARNESS_PATHS})
        for record in records:
            if record['gate'] not in self.gates: self.gates[record['gate']]=record
    def nested(self,case,obligations):
        records=[]
        for obligation in obligations:
            name=obligation['id']
            record={'case':case,'obligation':name,'configuration':self.s.target,
                'expected':obligation['expected'],'bound':obligation['bound'],
                'attempted':False,'status':'invalid'}
            binding=HANDOFF_BINDINGS[case].get(name)
            if binding is None:
                record['reason']='required control not implemented: '+name
            else:
                gate,field=binding; evaluated=self.gates.get(gate)
                if evaluated is None:
                    record['reason']='missing evaluated gate '+gate
                else:
                    record.update(at=evaluated['at'],attempted=evaluated['attempted'],gate=gate,
                        gate_artifact=evaluated.get('artifact'),status=evaluated['status'])
                    if evaluated['status']=='pass':
                        try:
                            record['evidence']=select_handoff_evidence(name,field,evaluated['evidence'])
                        except Exception as exc: record.update(status='invalid',reason=str(exc))
                    else:
                        # A gate raising early did not return proof for every
                        # nested claim. Preserve the actual FAIL at the gate,
                        # but do not fabricate dozens of failing assertions.
                        record.update(status='invalid',gate_status=evaluated['status'],
                            reason='gate did not return this obligation evidence',
                            gate_failure={k:v for k,v in evaluated.items() if k in ('assertion','reason','at')})
            records.append(record)
        return records


PATH_STIMULI={'clock-success-transcript':'success','clock-failure-transcript':'clock-readback-fail',
    'flash-init-failure-transcript':'flash-timing-fail','first-image-read-failure':'image-first-read-fail',
    'second-image-read-failure':'image-second-read-fail','target-init-reject':'boot_handoff_target_init',
    'observe-reject':'boot_handoff_target_observe','prepare-reject':'boot_startup_prepare'}
SUPPORT_ASSERTIONS={
    'stmdb-architectural-order':('STMDB exact predecrement/writeback','STMDB architectural register order'),
    'stmdb-alias-controls':('STMDB architectural register order',),
    'stmdb-illegal-controls':('intended illegal-list rejection','non-SP base reason'),
    'mutate-unknown-mmio-read':('unknown MMIO exact reason','exhausted MMIO rejection'),
    'mutate-mmio-leaf-semantics':('leaf control reason',),
    'corrupt-interpreter-branch':('interpreter branch expectation:',),
    'corrupt-interpreter-field-offset':('interpreter field-offset expectation:',),
    'corrupt-interpreter-copy':('interpreter copy expectation:',),
    'corrupt-interpreter-barrier-recording':('interpreter barrier-recording expectation:',),
    'mutate-ordinary-ram-borrowing':('ordinary BSS pointer:','constructed ordinary pointer:'),
    'mutate-terminal-vtor':('terminal VTOR:',),
    'mutate-terminal-old-stack':('terminal old stack:',),
    'mutate-terminal-barrier':('terminal barrier 6c60:','terminal barrier 6c68:',
        'terminal barrier 6c6c:','terminal barrier 6c80:','terminal barrier 6c88:'),
    'mutate-terminal-parameter':('wrapper guard/pointer/offset:',),
    'mutate-initializer-overlap':('initializer overlap:','initializer rounded overlap:'),
    'mutate-native-abi-offset':('native callback offset:',)}
CONTROL_STIMULI={'mutate-guard-sequence-status':'reject-sequence-status',
    'mutate-guard-sequence-stage':'reject-sequence-stage',
    'mutate-guard-watchdog-wrapper-status':'reject-watchdog-wrapper-status',
    'mutate-guard-runtime-valid':'reject-missing-runtime-valid',
    'mutate-guard-handoff-valid':'reject-missing-handoff-valid',
    'mutate-guard-nested-runtime-status':'reject-nested-runtime-status',
    'mutate-guard-handoff-status':'reject-handoff-status',
    'mutate-runtime-pointer':'runtime-pointer','mutate-launch-pointer':'launch-pointer',
    'mutate-launch-retry':'launch-retry','mutate-descriptor-binding':'descriptor',
    'mutate-context-binding':'producer-context','mutate-publication-byte':'publication-byte',
    'mutate-stop-dmb':'stop-barrier','mutate-ready11-dmb':'ready11-barrier',
    'mutate-ready13-dmb':'ready13-barrier','mutate-unknown-mmio-address':'unknown-mmio',
    'mutate-original-result-pointer':'original-result-pointer',
    'mutate-publication-order':'publication-order'}


def select_handoff_evidence(name,field,evidence):
    if field not in evidence or evidence[field] is None or evidence[field]==[] or evidence[field]=={}:
        raise Invalid('missing semantic evidence '+name+': '+field)
    value=evidence[field]
    if name in SUPPORT_ASSERTIONS:
        assertions=evidence.get('assertions',[])
        selected=[]
        for fragment in SUPPORT_ASSERTIONS[name]:
            matches=[a for a in assertions if fragment in a.get('assertion','') and a.get('matched') is True]
            if not matches: raise Invalid('missing evaluated handwritten assertion '+name+': '+fragment)
            selected.extend(matches)
        return {'selector':'handwritten oracle assertions','value':selected}
    stimulus=PATH_STIMULI.get(name,name if name.startswith('reject-') else None)
    if stimulus:
        value=[p for p in value if p.get('static_path_stimulus')==stimulus]
        if len(value)!=1: raise Invalid('missing/duplicate exact path '+stimulus)
    if name in CONTROL_STIMULI:
        value=[c for c in value if c.get('mutation')==CONTROL_STIMULI[name]]
        if len(value)!=1 or not value[0].get('rejected_by'):
            raise Invalid('missing/duplicate intended mutation rejection '+name)
    return {'selector':field,'value':value}


def reconcile_handoff(planned,records):
    """Pure reconciliation: failures survive later invalidity; no fifth pass."""
    bycase={c:[r for r in records if r.get('case')==c] for c in HANDOFF_CASES}
    counts={status:sum(r.get('status')==status for r in records if r.get('case') in HANDOFF_CASES)
            for status in ('pass','fail','invalid')}
    missing=[c for c in HANDOFF_CASES if not bycase[c]]
    duplicates=[c for c in HANDOFF_CASES if len(bycase[c])>1]
    trusted=(all(r.get('case') in HANDOFF_CASES and r.get('status') in ('pass','fail','invalid')
                 for r in records) and
             all(len(bycase[c])==1 and bycase[c][0].get('status')=='pass' and
                 bycase[c][0].get('trusted') is True for c in HANDOFF_CASES))
    return {'counts':counts,'missing':missing,'duplicates':duplicates,
        'composite_accepted':list(planned)==list(HANDOFF_CASES) and trusted and not missing and not duplicates}

def reconcile_nested(planned,records):
    ids=[r.get('obligation') for r in records]
    missing=[n for n in planned if n not in ids]
    duplicates=sorted({n for n in ids if ids.count(n)>1},key=str)
    unknown=[n for n in ids if n not in planned]
    counts={status:sum(r.get('status')==status for r in records) for status in ('pass','fail','invalid')}
    complete=(not missing and not duplicates and not unknown and len(planned)==len(set(planned)) and
              all(r.get('status')=='pass' and r.get('attempted') is True and
                  bool(r.get('evidence')) for r in records))
    return {'counts':counts,'missing':missing,'duplicates':duplicates,'unknown':unknown,'verified':complete}


def handoff_reporting_selfchecks():
    # Handwritten outcome rosters and expected metadata, not evaluator output.
    good=[{'case':'entry-inline-handoff','status':'pass','trusted':True},
          {'case':'retained-callback-closure','status':'pass','trusted':True},
          {'case':'terminal-transition','status':'pass','trusted':True},
          {'case':'ftpa-initializer-native-abi','status':'pass','trusted':True}]
    oracle(reconcile_handoff(HANDOFF_CASES,good)=={
        'counts':{'pass':4,'fail':0,'invalid':0},'missing':[],'duplicates':[],
        'composite_accepted':True},'four trusted representatives, composite not fifth pass')
    invalid=[{'case':'entry-inline-handoff','status':'invalid','trusted':False},*good[1:]]
    oracle(reconcile_handoff(HANDOFF_CASES,invalid)=={
        'counts':{'pass':3,'fail':0,'invalid':1},'missing':[],'duplicates':[],
        'composite_accepted':False},'entry invalid leaves all independent witnesses recorded')
    failure=[{'case':'entry-inline-handoff','status':'fail','trusted':False},
             {'case':'retained-callback-closure','status':'invalid','trusted':False},*good[2:]]
    oracle(reconcile_handoff(HANDOFF_CASES,failure)=={
        'counts':{'pass':2,'fail':1,'invalid':1},'missing':[],'duplicates':[],
        'composite_accepted':False},'valid failure survives later invalidity')
    oracle(reconcile_handoff(HANDOFF_CASES,good[:-1])=={
        'counts':{'pass':3,'fail':0,'invalid':0},'missing':['ftpa-initializer-native-abi'],
        'duplicates':[],'composite_accepted':False},'missing witness')
    oracle(reconcile_handoff(HANDOFF_CASES,[*good,good[0]])=={
        'counts':{'pass':5,'fail':0,'invalid':0},'missing':[],
        'duplicates':['entry-inline-handoff'],'composite_accepted':False},'duplicate witness')
    untrusted=[dict(good[0],trusted=False),*good[1:]]
    oracle(not reconcile_handoff(HANDOFF_CASES,untrusted)['composite_accepted'],'untrusted pass')
    oracle(not reconcile_handoff(HANDOFF_CASES[:1],good)['composite_accepted'],'subset cannot accept composite')
    oracle(not reconcile_handoff(HANDOFF_CASES,[*good,{'case':'unknown','status':'pass'}])['composite_accepted'],
           'unknown parent cannot pass')
    literal=[{'obligation':'a','status':'pass','attempted':True,'evidence':{'pc':256}},
             {'obligation':'b','status':'pass','attempted':True,'evidence':{'pc':260}}]
    oracle(reconcile_nested(['a','b'],literal)=={'counts':{'pass':2,'fail':0,'invalid':0},
        'missing':[],'duplicates':[],'unknown':[],'verified':True},'literal exact nested roster')
    for records,field,want in ((literal[:1],'missing',['b']),([*literal,literal[0]],'duplicates',['a']),
            ([*literal,{'obligation':'x','status':'pass'}],'unknown',['x'])):
        result=reconcile_nested(['a','b'],records)
        oracle(result[field]==want and not result['verified'],'nested '+field)
    for change in ({'status':'unknown'},{'status':'invalid'},{'evidence':{}},{'attempted':False}):
        oracle(not reconcile_nested(['a','b'],[dict(literal[0],**change),literal[1]])['verified'],
               'nested unknown/invalid/absent/unattempted evidence')
    result=reconcile_nested(['a','b'],[dict(literal[0],status='fail'),dict(literal[1],status='invalid')])
    oracle(result['counts']=={'pass':0,'fail':1,'invalid':1} and not result['verified'],
           'nested failure preserved alongside invalid')


def retain_handoff_runs(s):
    """Last five completed per real target, plus all active/protected proof."""
    candidates=[p for p in sorted(s.out.parent.glob('*-'+s.target)) if p.is_dir() and not p.is_symlink()]
    completed=[p for p in candidates if not (p/'.active').exists()]
    for old in completed[:-5]:
        if old!=s.out and not (old/'.preserve').exists(): shutil.rmtree(old)


def execute_handoff(s,callback,setup,finalize,roster,evidence):
    """A local FAIL/INVALID cannot suppress another representative or subgate."""
    common=None; nested=[]; reconciliations={}
    (s.out/'.active').touch(exist_ok=False)
    try: setup()
    except Exception as exc: common=str(exc)
    selected=[] if s.args.bringup_only else s.roster
    for case in selected:
        record={'case':case,'variant':'linked-static-v1',
                'configuration':roster.get('configuration'),'trusted':False,'attempted':False}
        callback_error=None
        if not common:
            record['attempted']=True
            try: callback(case)
            except Exception as exc: callback_error=exc
        obligations=roster.get('obligations',{}).get(case,[])
        outcomes=evidence.nested(case,obligations)
        nested.extend(outcomes)
        reconciliation=reconcile_nested([o['id'] for o in obligations],outcomes)
        reconciliations[case]=reconciliation
        gates=[evidence.gates[n] for n in evidence.case_gates[case]]
        failures=[g for g in gates if g['status']=='fail']
        if failures or isinstance(callback_error,Failure):
            failure=failures[0] if failures else {'at':callback_error.at,'assertion':callback_error.assertion}
            record.update(status='fail',at=failure['at'],assertion=failure['assertion'])
        elif common or callback_error or not obligations or not reconciliation['verified'] or AUTHORING_GAPS[case]:
            record.update(status='invalid',reason=common or str(callback_error or
                'nested obligations missing, invalid, untrusted or unimplemented'))
        else: record.update(status='pass',trusted=True)
        if case=='retained-callback-closure':
            record['composite_dependencies']=['entry-inline-handoff:adapter-and-retained-context-bindings']
        record['nested_reconciliation']=reconciliation
        record['gate_outcomes']=[{k:v for k,v in g.items() if k!='evidence'} for g in gates]
        s.emit(record)
    completion_error=None
    try: finalize()
    except Exception as exc: completion_error=str(exc)
    if time.monotonic()>s.deadline:
        completion_error='allocated deadline exhausted including identity/evidence handling'
    result=reconcile_handoff(selected,s.records)
    if common or completion_error or s.args.bringup_only: result['composite_accepted']=False
    support_ok=not common and not completion_error
    if s.args.bringup_only:
        result['support_status']='complete' if support_ok else 'invalid'
        result['product_credit']=False
    if any(n['status']!='pass' for n in nested): result['composite_accepted']=False
    immutable_json(s.out/'gate-evidence.json',evidence.gates)
    with (s.out/'witness.jsonl').open('x') as stream:
        for record in nested: stream.write(json.dumps(record,sort_keys=True)+'\n')
    result.update(planned=list(HANDOFF_CASES),selected=selected,
        attempted=[r['case'] for r in s.records if r.get('attempted')],
        completed=[r['case'] for r in s.records if r.get('attempted')],
        verified=[r['case'] for r in s.records if r.get('trusted')],
        reconciliation=reconciliations,support_error=common,completion_error=completion_error,
        authoring_gaps=AUTHORING_GAPS)
    s.manifest.update(result)
    immutable_json(s.out/'manifest.json',s.manifest)
    s.emit({'summary':result})
    s.emit({'rollup':{**result['counts'],'representatives':4,'composite_accepted':result['composite_accepted']}})
    s.log.close(); s.report.close()
    (s.out/'.active').unlink()
    retain_handoff_runs(s)
    return int(not (support_ok if s.args.bringup_only else result['composite_accepted']))
