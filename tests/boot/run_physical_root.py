#!/usr/bin/env python3
"""Proposal-authored OFFLINE physical adapter / PUBLIC root gate. Never target-executes."""
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
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.boot_fingerprints import source_fingerprints

FIX=ROOT/'tests/boot/physical_root'
GCC=Path('/Users/nhan/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc')
CC=Path('/usr/bin/clang')
CASES=('safe-args','physical-map','init-clock','images-publication','bridge-bind',
       'root-reject','root-rebuild','verifier-roots','arm-closure','shared-signed-run','shared-signature-reject')
# Public-only RFC6979 A.2.5 Q and SEC2 secp256r1 base point, NOT generated keys.
RFC=bytes.fromhex('0460FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB67903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299')
SEC2=bytes.fromhex('046B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C2964FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5')
BASE_VERIFY='bc63e3c02f91eeb32858ba85313ab7ae7255d23ee318b58010e2a2a64d54abca'
class Invalid(Exception): pass
class Failure(Exception):
    def __init__(self, message, at=None):
        super().__init__(message)
        self.at=at or 'tests/boot/run_physical_root.py:0'


def require(ok, msg):
    if not ok: raise Failure(msg, 'tests/boot/run_physical_root.py:'+str(inspect.currentframe().f_back.f_lineno))
def oracle(ok,msg):
    if not ok: raise Invalid('oracle: '+msg)
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def symbols(text):
    out=[]
    for line in text.splitlines():
        if not line.strip(): continue
        m=re.fullmatch(r'([0-9a-fA-F]+) ([A-Za-z]) (\S+)',line)
        if not m: raise Invalid('unknown nm record '+line[:120])
        out.append((m[3],m[2],int(m[1],16)))
    return out

def objects(text):
    rows=[]
    for line in text.splitlines():
        fields=line.split()
        if len(fields) in (6,7) and fields[2]=='O' and (len(fields)==6 or fields[5]=='.hidden'):
            rows.append((fields[-1],fields[3],int(fields[4],16),int(fields[0],16)))
        elif re.search(r'\bO\s+\.',line): raise Invalid('unparsed object row '+line)
    return rows

def sections(text):
    rows=[]; lines=text.splitlines()
    for i,line in enumerate(lines):
        m=re.fullmatch(r'[ \t]*[0-9]+[ \t]+(\S+)[ \t]+([0-9a-fA-F]+)[ \t]+([0-9a-fA-F]+)[ \t]+[0-9a-fA-F]+[ \t]+[0-9a-fA-F]+[ \t]+2\*\*[0-9]+[ \t]*',line)
        if m:
            if i+1>=len(lines): raise Invalid('section flags missing')
            rows.append((m[1],int(m[2],16),int(m[3],16),tuple(x.strip() for x in lines[i+1].split(','))))
    if not rows: raise Invalid('no parsed section headers')
    return rows

def c_bytes(text):
    if text.count('/*') != text.count('*/'): raise Invalid('unterminated PUBLIC comment')
    text=re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)
    arrays=re.findall(r'\buint8_t\s+\w+\s*\[[^\]]+\]\s*=\s*\{([^{}]*)\}\s*;',text)
    if len(arrays)!=1: raise Invalid('expected one PUBLIC uint8_t initializer')
    body=arrays[0].strip()
    if not re.fullmatch(r'0x[0-9a-fA-F]{2}(?:\s*,\s*0x[0-9a-fA-F]{2})*\s*,?',body):
        raise Invalid('malformed PUBLIC initializer')
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',body))
def extracted(text):
    return re.findall(r'^(\S+\.a)\(([^()]+)\)$',text.split('Discarded input sections',1)[0],re.M)
def diagnostic(text): return ' '.join(text.split())
def dependencies(text): return text.replace('\\\n',' ').split(':',1)[1].split()
def access_copy():
    source=(ROOT/'src/boot_target_io.c').read_text()
    replacements={
      '*(volatile const uint32_t *)(uintptr_t)TARGET_RESC':'*fixture_word(TARGET_RESC, 0)',
      '*(volatile uint32_t *)(uintptr_t)TARGET_RESC':'*fixture_word(TARGET_RESC, 1)',
      '*(volatile const uint32_t *)(uintptr_t)address':'*fixture_word(address, 0)',
      '*(volatile uint32_t *)(uintptr_t)address':'*fixture_word(address, 1)',
      '*(volatile const uint8_t *)(uintptr_t)(address + (uint32_t)i)':'*fixture_byte(address + (uint32_t)i)'}
    for old,new in replacements.items():
        if source.count(old)!=1: raise Invalid('safe access leaf changed: '+old)
        source=source.replace(old,new)
    if re.search(r'\*\s*\([^\n;]*uintptr_t',source): raise Invalid('unmapped physical dereference remains')
    return '#include "safe_access.h"\n'+source

def main():
    p=argparse.ArgumentParser(description=__doc__)
    s=p.add_mutually_exclusive_group(required=True); s.add_argument('--all',action='store_true'); s.add_argument('--case',action='append',choices=CASES)
    p.add_argument('--budget-seconds',type=int,required=True,choices=range(1,601))
    p.add_argument('--baseline-manifest',type=Path,help='Prior bring-up manifest; require exact source/tool continuity')
    p.add_argument('--human',action='store_true'); p.add_argument('--failures-only',action='store_true')
    a=p.parse_args()
    if a.case and len(set(a.case))!=len(a.case): p.error('duplicate case')
    roster=[c for c in CASES if a.all or c in a.case]
    base=ROOT/'.dev-vault/tests/runs'; out=base/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-physical-root'); out.mkdir(parents=True)
    work=Path(tempfile.mkdtemp(prefix='physical-root-')) # production forbids source-tree build outputs
    logpath=out/'run.log'; reportpath=out/'report.jsonl'
    deadline=time.monotonic()+a.budget_seconds; start=time.monotonic()
    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',ASAN_OPTIONS='halt_on_error=1:detect_stack_use_after_return=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    records=[]; prepared=set(); stopped=None; before={}; after={}; tools_before={}; tools_after={}
    def identity():
        return source_fingerprints(ROOT)
    # Tool bundle includes assembler/linker/cc1/libgcc/libc/specs used transitively.
    cmake=shutil.which('cmake'); make=shutil.which('make')
    def tool_identity():
        if not cmake or not make or not GCC.is_file(): raise Invalid('CMake/make/official Arm15.3 bundle missing; no Arm PATH fallback')
        files={CC,Path(cmake),Path(make),Path(os.sys.executable)}
        files.update(f for f in GCC.parent.parent.rglob('*') if f.is_file())
        # Apple clang driver is pinned; record local xcrun/SDK tool identity if present.
        for tool in ('ar','ranlib','ld','xcrun'):
            if shutil.which(tool): files.add(Path(shutil.which(tool)))
        return {str(f):sha(f) for f in sorted(files)}
    metadata={'roster':roster,'work':str(work),'configurations':['HOST O1 ASan+UBSan','ARM Release Os synthetic static closure'],
              'nested':{'root-reject':['unspecified','missing-file','malformed','offcurve','demo'], 'root-rebuild':['RFC-selected','SEC2-build-auto-reconfigure'], 'verifier-roots':['production-demo-reject','direct-missing-header-reject']}}
    with logpath.open('wb') as log, reportpath.open('w') as report:
        def emit(r):
            records.append(r); line=json.dumps(r,separators=(',',':')); report.write(line+'\n'); report.flush()
            if not a.human and (not a.failures_only or r.get('status')!='pass'): print(line,flush=True)
        def run(argv, behavior=False, expected_error=False, bounded=True):
            remaining=deadline-time.monotonic()
            if remaining<=0: raise Invalid('invocation deadline exhausted')
            argv=list(map(str,argv)); log.write(('\n$ '+repr(argv)+'\n').encode()); log.flush(); pos=log.tell()
            try:
                process=subprocess.Popen(argv,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                try: process.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL); process.wait()
                    raise Invalid('invocation deadline exhausted; process group killed')
                result=process
            except (OSError,subprocess.SubprocessError) as e: raise Invalid(str(e)) from e
            log.flush(); size=log.tell()-pos
            if not bounded:
                if result.returncode: raise Invalid('artifact command exit '+str(result.returncode))
                return ''
            with logpath.open('rb') as f: f.seek(pos); body=f.read(size).decode(errors='replace') if size<=65536 else ''
            if size>65536: raise Invalid('parser input exceeds 64KiB; raw preserved')
            if expected_error:
                require(result.returncode!=0,'invalid production root must be rejected')
            elif result.returncode:
                if behavior:
                    if 'AddressSanitizer: stack-overflow' in body or 'AddressSanitizer:DEADLYSIGNAL' in body: raise Invalid('sanitizer launch/storage failure; raw retained')
                    match=re.search(r'([^\s:]+\.(?:c|h)):(\d+):[ \t]*([^\n]+)',body)
                    if match: raise Failure(match[3],match[1]+':'+match[2])
                    raise Failure(body.strip()[:240] or 'behavior exit '+str(result.returncode))
                raise Invalid('build/tool exit '+str(result.returncode)+'; see run.log')
            return body
        def configure(build, production=False, key=None, arm=False, invalid=False):
            args=[cmake,'-S',FIX,'-B',build,'-G','Unix Makefiles','-DCMAKE_MAKE_PROGRAM='+str(make),'-DPython3_EXECUTABLE='+str(Path(os.sys.executable).resolve()),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER='+str(GCC if arm else CC),'-DLM3S6965_BOOT_PHYSICAL_PRODUCTION='+('ON' if production else 'OFF')]
            if key is not None: args+=['-DLM3S6965_BOOT_PUBLIC_KEY_FILE='+str(key)]
            if arm:
                args+=['-DCMAKE_SYSTEM_NAME=Generic','-DCMAKE_SYSTEM_PROCESSOR=arm','-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY','-DCMAKE_ASM_COMPILER='+str(GCC)]
                for name in ('AR','RANLIB','OBJCOPY','OBJDUMP','SIZE','NM'):
                    args+=['-DCMAKE_'+name+'='+str(GCC.parent/('arm-none-eabi-'+name.lower()))]
            else: args+=['-DADAPTER_COPY='+str(work/'safe_adapter.c')]
            return run(args,expected_error=invalid)
        def build(directory,*targets): run([cmake,'--build',directory,'--parallel','2','--target',*targets])
        def host():
            if 'host' not in prepared:
                configure(work/'host'); build(work/'host','adapter_host'); prepared.add('host')
        def public_file(name,key):
            f=work/(name+'.sec1'); f.write_bytes(key); return f
        def generated(builddir): return builddir/'generated/boot-public-root/lm3s6965_boot_production_public_key.h'
        def root_reject():
            demo=c_bytes((ROOT/'config/boot_demo_public_key.h').read_text()); oracle(len(demo)==65,'demo public parsing length')
            scenarios=[('unspecified',None,'Production requires explicit'),('missing-file',work/'absent.sec1','PUBLIC key file is missing'),('malformed',public_file('malformed',RFC[:-1]),'expected exactly 65 raw'),('offcurve',public_file('offcurve',b'\x04'+bytes(64)),'not on P-256'),('demo',public_file('demo',demo),'demo PUBLIC key is forbidden')]
            for name,key,diagnostic in scenarios:
                directory=work/('reject-'+name); header=generated(directory); header.parent.mkdir(parents=True); header.write_text('STALE ROOT')
                body=configure(directory,True,key,invalid=True)
                require(diagnostic in ' '.join(body.split()),'specific rejection reason: '+name)
                require(not header.exists(),'failed configuration removes stale root: '+name)
        def root_rebuild():
            directory=work/'rebuild'; key=public_file('changing-public',RFC)
            body=configure(directory,True,key); expected=hashlib.sha256(RFC).hexdigest()
            require('Production selected PUBLIC root SHA256: '+expected in body,'exact selected RFC public fingerprint')
            require(c_bytes(generated(directory).read_text())==RFC,'generated exact RFC bytes')
            build(directory,'root_behavior')
            # Locate CMake object by suffix, rather than assuming CMake's absolute path encoding.
            candidates=list((directory/'CMakeFiles/verifier.dir').rglob('boot_verify.c.o'))
            if len(candidates)!=1: raise Invalid('expected one verifier object')
            obj=candidates[0]; old=(sha(obj),obj.stat().st_mtime_ns); header=generated(directory)
            require(RFC in obj.read_bytes() and SEC2 not in obj.read_bytes(),'initial object contains selected RFC and excludes future SEC2')
            shutil.copy2(obj,out/'verifier-before.o')
            # Deliberately same integer second as the existing object, no sleep.
            # This exposes stale-trust Make timestamp granularity without hiding it.
            key.write_bytes(SEC2); stamp=(old[1]//1_000_000_000)*1_000_000_000+999_999_999; os.utime(key,ns=(stamp,stamp))
            require(key.stat().st_mtime_ns//1_000_000_000==old[1]//1_000_000_000,'replacement and previous object share integer second')
            body=run([cmake,'--build',directory,'--parallel','2','--target','root_behavior'])
            require('Production selected PUBLIC root SHA256: '+hashlib.sha256(SEC2).hexdigest() in body,'key change triggers CMAKE_CONFIGURE_DEPENDS during build without explicit configure')
            require(c_bytes(header.read_text())==SEC2,'regenerated exact changed PUBLIC bytes')
            require(sha(obj)!=old[0] and obj.stat().st_mtime_ns!=old[1],'concrete verifier object rebuilt with new key')
            require(SEC2 in obj.read_bytes() and RFC not in obj.read_bytes(),'rebuilt verifier contains actual selected SEC2 and excludes old RFC')
            flags=(directory/'CMakeFiles/verifier.dir/flags.make').read_text()
            require('LM3S6965_BOOT_PUBLIC_ROOT_SHA256' in flags and hashlib.sha256(SEC2).hexdigest() in flags and expected not in flags,'compiler command identity uses changed selected SHA256')
            shutil.copy2(obj,out/'verifier-after.o')
            (out/'root-freshness.json').write_text(json.dumps({'before_sha256':old[0],'after_sha256':sha(obj),'before_mtime_ns':old[1],'after_mtime_ns':obj.stat().st_mtime_ns,'replacement_mtime_ns':stamp,'selected_present':True,'old_absent':True,'compile_identity_changed':True},indent=2)+'\n')
            require(run([directory/'root_behavior'],behavior=True).splitlines()==['ROOT_BEHAVIOR_OK'],'rebuilt production still rejects demo signed fixture')
            shutil.copy2(header,out/'rebuilt-public-root.h')
        def verifier_roots():
            directory=work/'production-host'; key=public_file('production-rfc',RFC)
            configure(directory,True,key); build(directory,'root_behavior')
            require(run([directory/'root_behavior'],behavior=True).splitlines()==['ROOT_BEHAVIOR_OK'],'alternate production root rejects demo signatures, unchanged output')
            # Production missing generated header must not fall back to demo.
            body=run([CC,'-std=c11','-DLM3S6965_BOOT_PHYSICAL_PRODUCTION=1','-I',ROOT/'include','-I',ROOT/'config','-I',ROOT/'third_party/mbedtls/include','-DMBEDTLS_CONFIG_FILE="mbedtls_boot.h"','-c',ROOT/'src/boot_verify.c','-o',work/'must-not-exist.o'],expected_error=True)
            require('lm3s6965_boot_production_public_key.h' in body and not (work/'must-not-exist.o').exists(),'direct production consumer fails closed without generated root')
        def arm_closure():
            require('15.3' in run([GCC,'--version']),'official Arm compiler version15.3')
            directory=work/'arm'; key=public_file('arm-rfc',RFC); configure(directory,True,key,True); build(directory,'adapter_closure')
            elf=directory/'adapter_closure'; nm=GCC.parent/'arm-none-eabi-nm'; dump=GCC.parent/'arm-none-eabi-objdump'
            require(not run([nm,'-u',elf]).strip(),'synthetic actual adapter link has no undefined symbols')
            table=symbols(run([nm,'-n','--defined-only',elf]))
            expected=('boot_target_reset_adapter','boot_target_flash_io','boot_target_flash_init','boot_target_images_read','boot_target_verify_read','boot_target_sequence_bind','boot_sequence_run','boot_flash_init','boot_flash_read','boot_flash_store_adapter','boot_startup_producer_begin_io','boot_runtime_watchdog_run','boot_reset_handler_run','boot_select','boot_verify_manifest','mbedtls_ecdsa_verify')
            for name in expected: require(sum(n==name and t in ('T','t') and v<0x100000 for n,t,v in table)==1,'one FLASH code definition '+name)
            adapter=[p for p in directory.rglob('boot_target_io.c.*') if p.suffix in ('.o','.obj')]
            require(len(adapter)==1,'actual adapter object compiled once')
            rows=objects(run([dump,'-t',adapter[0]]))
            require(not [r for r in rows if r[2] and not r[1].startswith('.rodata')],'adapter has no mutable static objects')
            section_rows=sections(run([dump,'-h',adapter[0]]))
            require(not [r for r in section_rows if r[1] and 'ALLOC' in r[3] and 'READONLY' not in r[3]],'actual adapter has zero nonempty writable allocated sections')
            elf_sections=sections(run([dump,'-h',elf]))
            require(any(n=='.text' and 'READONLY' in flags and 'CODE' in flags for n,size,addr,flags in elf_sections),'root/code output section is actually read-only code')
            rows=objects(run([dump,'-t',elf])); roots=[r for r in rows if r[0]=='lm3s6965_boot_public_key']
            require(len(roots)==1 and roots[0][1]=='.text' and roots[0][2]==65 and roots[0][3]<0x100000,'exact immutable public root object in FLASH section')
            require(not any('demo_public_key' in n for n,_,_ in table),'no demo root symbol')
            demo=c_bytes((ROOT/'config/boot_demo_public_key.h').read_text())
            require(demo not in elf.read_bytes() and RFC in elf.read_bytes(),'production ELF contains selected PUBLIC bytes and no demo bytes')
            # Include dependency closure of the actual verifier object, not just a lookalike symbol.
            dep=[p for p in (directory/'CMakeFiles/verifier.dir').rglob('boot_verify.c.*') if p.name.endswith(('.o.d','.obj.d'))]
            if len(dep)!=1: raise Invalid('missing verifier include dependencies')
            paths=dependencies(dep[0].read_text())
            require(any(x.endswith('lm3s6965_boot_production_public_key.h') for x in paths) and not any(x.endswith('boot_demo_public_key.h') for x in paths),'production verifier include closure excludes demo')
            run([dump,'-dr',elf],bounded=False)
            shutil.copy2(elf,out/'adapter-synthetic.elf'); shutil.copy2(directory/'closure.map',out/'adapter-synthetic.map')
            shutil.copy2(adapter[0],out/'actual-adapter.o')
            # Preserve exact extracted prebuilt members/exports/attributes for a
            # signature-level ABI assessment, not a blanket archive claim.
            helpers=[]
            for archive,member in extracted((directory/'closure.map').read_text()):
                if Path(archive).name not in ('libc.a','libgcc.a','libnosys.a'): continue
                memberfile=out/(Path(archive).stem+'-'+member)
                with memberfile.open('wb') as dest:
                    remaining=deadline-time.monotonic()
                    if remaining<=0: raise Invalid('deadline before prebuilt ABI evidence')
                    result=subprocess.run([str(GCC.parent/'arm-none-eabi-ar'),'p',archive,member],stdout=dest,stderr=log,timeout=remaining)
                if result.returncode: raise Invalid('prebuilt member extraction failed')
                exports=symbols(run([nm,'-n','--defined-only',memberfile]))
                attrs=run([GCC.parent/'arm-none-eabi-readelf','-A',memberfile])
                helpers.append({'archive':str(Path(archive).resolve()),'member':member,'sha256':sha(memberfile),'symbols':exports,'attributes':attrs})
            oracle(bool(helpers),'extracted prebuilt helper roster nonempty')
            (out/'prebuilt-abi.json').write_text(json.dumps(helpers,indent=2)+'\n')
        def shared(case):
            # Changed verifier dependency justifies precisely two existing assertion intents.
            require(sha(ROOT/'src/boot_verify.c')!=BASE_VERIFY,'shared regression requires verifier delta from accepted prior gate')
            if 'shared' not in prepared:
                run([cmake,'-S',ROOT/'tests/boot','-B',work/'shared','-G','Unix Makefiles','-DCMAKE_MAKE_PROGRAM='+str(make),'-DPython3_EXECUTABLE='+str(Path(os.sys.executable).resolve()),'-DCMAKE_C_COMPILER='+str(CC),'-DCMAKE_BUILD_TYPE=Release','-DBOOT_SANITIZERS=ON'])
                build(work/'shared','test_sequence'); prepared.add('shared')
            require(run([work/'shared/test_sequence',case],behavior=True).splitlines()==['SEQUENCE_CASE_OK '+case],'unchanged existing shared '+case+' exact completion')
        try:
            # Bring-up support checks: captured-format exact tuple oracles, not logical reps.
            oracle(extracted('Archive member included\n/x/libc.a(libc_a-memcmp.o)\n  caller (memcmp)\nDiscarded input sections\n/x/libgcc.a(not-extracted.o)\n')==[('/x/libc.a','libc_a-memcmp.o')],'map extraction exact archive/member tuple excludes discarded records')
            oracle(diagnostic('Production PUBLIC key file is\n  missing or not a regular file')=='Production PUBLIC key file is missing or not a regular file','CMake wrapped diagnostic preserves exact reason')
            oracle('PUBLIC key file is missing' not in diagnostic('PUBLIC key file is invalid'),'diagnostic does not conflate rejection reasons')
            oracle(re.search(r'([^\s:]+\.(?:c|h)):(\d+):[ \t]*([^\n]+)', 'adapter_host.c:17: expected == actual').groups()==('adapter_host.c','17','expected == actual'),'failure location exact tuple')
            oracle(symbols('00000010 T boot_sequence_run\n00000020 R root\n')==[('boot_sequence_run','T',16),('root','R',32)],'nm exact tuples')
            oracle(objects('00000020 l     O .rodata.root 00000041 root\n00000000 l    d  .data 00000000 .data\n')==[('root','.rodata.root',65,32)],'object type/section/size/address tuple')
            oracle(sections('  0 .text 00000010 00000000 00000000 00000034 2**2\n                  CONTENTS, ALLOC, LOAD, READONLY, CODE\n')==[('.text',16,0,('CONTENTS','ALLOC','LOAD','READONLY','CODE'))],'section exact name/size/address/flags tuple')
            oracle(c_bytes('/* SEC1 0x04 prefix */ static const uint8_t x[SIZE]={0x04, 0x0a, 0xff};')==bytes([4,10,255]),'captured PUBLIC initializer excludes comment')
            oracle(c_bytes('static const uint8_t x[3]={0x04, 0x0a, 0xff,};')==bytes([4,10,255]),'generated PUBLIC initializer exact bytes')
            for bad in ('/* open', 'uint8_t x[1]={0x04', 'uint8_t x[1]={0x04, nope};', 'uint8_t x[1]={0x04}; uint8_t y[1]={0x05};'):
                try: c_bytes(bad)
                except Invalid: pass
                else: raise Invalid('oracle: malformed PUBLIC initializer accepted')
            oracle(dependencies('x.o: a.c \\\n b.h\n')==['a.c','b.h'],'dependency exact paths')
            oracle(('ADAPTER_CASE_OK safe-args\n'.splitlines()==['ADAPTER_CASE_OK safe-args']) and ('ADAPTER_CASE_OK wrong\n'.splitlines()!=['ADAPTER_CASE_OK safe-args']),'completion positive/negative')
            before=identity(); tools_before=tool_identity()
            if a.baseline_manifest:
                previous=json.loads(a.baseline_manifest.read_text())
                if not previous.get('complete') or previous['counts']['fail'] or previous['counts']['invalid']:
                    raise Invalid('baseline bring-up incomplete or non-pass')
                if before!=previous['inputs_after'] or tools_before!=previous['tools_after']:
                    raise Invalid('source/tool identity differs from bring-up; no implicit continuation')
                metadata['baseline_manifest']=str(a.baseline_manifest)
            (work/'safe_adapter.c').write_text(access_copy()); shutil.copy2(work/'safe_adapter.c',out/'safe_adapter.c')
            oracle(RFC.hex().upper() in ''.join(re.findall(r'"([0-9A-F]+)"',(ROOT/'tests/boot/test_crypto.c').read_text())),'RFC public fixture matches existing public-only KAT')
        except Exception as e: stopped=str(e)
        for case in roster:
            if stopped:
                emit({'case':case,'status':'invalid','reason':'not attempted after infrastructure failure: '+stopped[:150]}); continue
            try:
                if case in CASES[:5]:
                    host(); require(run([work/'host/adapter_host',case],behavior=True).splitlines()==['ADAPTER_CASE_OK '+case],'exact host case completion')
                elif case=='root-reject': root_reject()
                elif case=='root-rebuild': root_rebuild()
                elif case=='verifier-roots': verifier_roots()
                elif case=='arm-closure': arm_closure()
                else: shared(case.removeprefix('shared-'))
                emit({'case':case,'status':'pass'})
            except Failure as e: emit({'case':case,'status':'fail','at':e.at,'assert':str(e)[:240]})
            except Exception as e:
                stopped=str(e); emit({'case':case,'status':'invalid','reason':stopped[:240]})
        try: after=identity(); tools_after=tool_identity()
        except Exception as e: stopped='postfreeze failed: '+str(e)
        if time.monotonic()>deadline:
            stopped='invocation wall budget exhausted (including fingerprint IO)'
            emit({'case':'run-completeness','status':'invalid','reason':stopped})
        if not before or before!=after or not tools_before or tools_before!=tools_after:
            emit({'case':'input-identity','status':'invalid','reason':'source/tool fingerprint unavailable or changed; preserve case evidence separately'})
        counts={s:sum(r.get('status')==s for r in records) for s in ('pass','fail','invalid')}
        metadata.update(inputs_before=before,inputs_after=after,tools_before=tools_before,tools_after=tools_after,elapsed_seconds=time.monotonic()-start,counts=counts,
                        complete=not stopped and before==after and tools_before==tools_after,
                        reconciliation={'planned':len(roster),'observed':sum(r.get('case') in roster for r in records),'missing':[c for c in roster if not any(r.get('case')==c for r in records)],'duplicates':[c for c in roster if sum(r.get('case')==c for r in records)!=1]},
                        nested_outcomes={c:{n:('pass' if any(r.get('case')==c and r.get('status')=='pass' for r in records) else 'incomplete-unlocalized') for n in names} for c,names in metadata['nested'].items() if c in roster})
        (out/'manifest.json').write_text(json.dumps(metadata,indent=2)+'\n')
        emit({'summary':{'target':'physical-root','kind':'python',**counts,'report':str(reportpath),'log':str(logpath)}})
        emit({'rollup':{**counts,'targets':1}})
    for old in sorted(base.glob('*-physical-root'))[:-5]:
        if old.is_dir() and not old.is_symlink(): shutil.rmtree(old)
    if a.human:
        with logpath.open() as f: shutil.copyfileobj(f,os.sys.stdout)
        print(reportpath.read_text(),end='')
    return int(bool(counts['fail'] or counts['invalid']))
if __name__=='__main__': raise SystemExit(main())
