#!/usr/bin/env python3
"""Proposal runner; explicit allocation required. No automatic retries or discovery."""
import argparse
from datetime import datetime, timezone
import hashlib
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

CLOCK = ['c_ready_validated_before_app', 'systick_admission_rejects_unsafe_core',
         'systick_poststart_pending_fails_closed', 'systick_offset_no_retained_io',
         'gptm_explicit_timer_claim_configuration', 'gptm_unknown_or_enabled_never_stops_timer',
         'gptm_claim_io_failure_consumed', 'gptm_expiry_before_tar', 'gptm_expiry_after_tar',
         'gptm_drift_rejects_unpublished_sample', 'gptm_claim_reentry_terminal',
         'gptm_read_reentry_terminal', 'gptm_release_consumed_no_doublewrite']
OWNER = ['owner_completed_turn_sequence', 'owner_timer_reentry_quarantines',
         'owner_partial_report_failure_no_retry', 'owner_ambiguous_feed_no_retry',
         'owner_watchdog_read_reentry_stops_before_icr']
NETIF = ['netif_completed_work_returns_true', 'netif_uninitialized_returns_false',
         'netif_phy_timeout_returns_false', 'netif_rx_error_returns_false']
TCP = ['ftp_one_step_then_reentry_sticky', 'ftp_missing_core_sticky']
SIGNED = 'target_authenticated_foreground_continuity'
ROSTERS = {
 'first': ['signed-bringup-build', 'native-clock-build'] + CLOCK + ['boot_startup',
    'main_systick_gptm_network_release_order_static', SIGNED, 'source-freeze-after'],
 'second': [f'{s}-{c}:{k}' for s in 'AB' for c in ('Debug','Release')
            for k in ('build','static')] + ['source-freeze-after'],
 'diagnostics': ['harness-diagnostics', 'source-freeze-after']}
SECONDS = {'first': 820, 'second': 400, 'diagnostics': 200}
CONFIGS = {
 'c_ready_validated_before_app': ['readiness-0','readiness-1','readiness-2','readiness-3'],
 'systick_admission_rejects_unsafe_core': ['mask','unknown-source','shcsr'],
 'gptm_explicit_timer_claim_configuration': ['timer-0','timer-1','timer-2','timer-3'],
 'gptm_release_consumed_no_doublewrite': ['normal','ris-reentry','ctl-reentry'],
 SIGNED: [c+'/'+s for c in ('bootOs/appOs-A','bootOs/appOg-B') for s in
          ('msp-clean','psp-clean','msp-masks','psp-masks','pre-c-clock-reject')]}


class Infrastructure(Exception): pass
class Contract(Exception):
    def __init__(self, at, message): self.at, self.message = at, message

def emit(record, stream):
    stream.write(json.dumps(record, separators=(',', ':'))+'\n'); stream.flush()

def freeze(root):
    # Explicit reuse of the reviewed identity ledger: no traversal/discovery.
    # The overlay changes only scoped harness inputs and the stated main move.
    expected = json.loads((AUTHOR / 'identity.json').read_text())['expected']
    result = {}
    for name in expected:
        author_inputs = {'@author-driver': 'phase-driver.py', '@intents': 'intents.json',
                         '@roster': 'roster.json', '@reuse-ledger': 'reuse-ledger.json'}
        if name == '@phase-runner':
            path = Path(__file__)
        elif name in author_inputs:
            path = AUTHOR / author_inputs[name]
        else:
            path = root / name
        data = ('link:'+os.readlink(path)).encode() if path.is_symlink() else path.read_bytes()
        result[name] = hashlib.sha256(data).hexdigest()
    return result

AUTHOR = Path(os.environ.get('CLOCK_SWITCH_AUTHOR', '/tmp/clock-switch-bootOs-author'))

class Run:
    def __init__(self, args, directory):
        self.args, self.directory = args, directory
        self.started = args.invocation_started
        self.deadline = self.started+SECONDS[args.phase]
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
            ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=0',
            UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
            FETCHCONTENT_FULLY_DISCONNECTED='ON')
        if args.arm_bin:
            self.env['PATH'] = str(args.arm_bin)+os.pathsep+self.env.get('PATH','')
        self.commands=[]; self.nested={}; self.infrastructure_broken=False
        self.log=(directory/'run.log').open('w')
    def command(self, argv, behavioral=False, marker=None, extra_env=None):
        left=self.deadline-time.monotonic()-5  # reserve final identity/reporting
        if left<=0: raise Infrastructure('phase deadline reached before command')
        command_started=time.monotonic()
        argv=list(map(str,argv)); path=self.directory/f'command-{len(self.commands):03}.log'
        env=dict(self.env); env.update(extra_env or {})
        self.log.write('$ '+json.dumps(argv)+'\n'); self.log.flush()
        with path.open('wb') as stream:
            try:
                p=subprocess.Popen(argv,cwd=self.args.root,env=env,stdout=stream,
                                   stderr=subprocess.STDOUT,start_new_session=True)
            except OSError as e: raise Infrastructure(str(e)) from e
            timed=False
            try: rc=p.wait(timeout=left)
            except subprocess.TimeoutExpired:
                timed=True; os.killpg(p.pid,signal.SIGKILL); rc=p.wait()
        self.commands.append({'argv':argv,'exit':rc,'timeout':timed,'elapsed_seconds':time.monotonic()-command_started,'log':str(path)})
        (self.directory/'commands.json').write_text(json.dumps(self.commands,indent=2)+'\n')
        with path.open() as stream: shutil.copyfileobj(stream,self.log)
        self.log.flush()
        # Inspect bounded evidence only. Full output is retained in run.log.
        with path.open('rb') as stream: raw=stream.read(1024*1024+1)
        text=raw.decode(errors='replace')
        if marker and marker.startswith('FOREGROUND_CASE_OK '):
            case=marker.split(' ',1)[1]
            observed=re.findall(r'^FOREGROUND_CONFIG_OK '+re.escape(case)+r' (\S+)$',text,re.M)
            self.nested[case]={'planned':CONFIGS.get(case,[]),'observed':observed}
        assertion=re.search(r'FOREGROUND_ASSERT (\S+:\d+) ([^\n]+)',text)
        legacy=re.search(r'(?:[^\n]*Assertion[^\n]*failed[^\n]*|[^\n]*Assertion failed[^\n]*|STARTUP FAIL[^\n]*)',text)
        # Only explicit sanitizer diagnostics attributable to a production source
        # location are product evidence. Runtime startup failures, DEADLYSIGNAL,
        # fixture faults and unsymbolized reports remain invalid, never invented.
        production = r'((?:[^\s:]*?/)?(?:src|examples|include)/[^\s:]+\.[ch]):(\d+)(?::\d+)?'
        ubsan = re.search(production+r': runtime error: ([^\n]+)', text)
        asan = re.search(r'ERROR: AddressSanitizer: (heap-buffer-overflow|stack-buffer-overflow|global-buffer-overflow|heap-use-after-free|stack-use-after-return|stack-use-after-scope|attempting double-free|alloc-dealloc-mismatch)[^\n]*', text)
        asan_summary = re.search(r'SUMMARY: AddressSanitizer: ([^\n]+)', text)
        # First fault stack only: never attribute an allocator/history frame to
        # the violating operation. Require a source-located top frame (#0).
        first_top = re.search(r'^\s*#0 [^\n]+', text, re.M)
        top = re.search(production, first_top[0]) if first_top else None
        sanitizer = None
        if ubsan:
            sanitizer = (ubsan[1]+':'+ubsan[2], 'UBSan runtime error: '+ubsan[3])
        elif asan and asan_summary and top:
            sanitizer = (top[1]+':'+top[2], asan[0])
        # Genuine assertion is preserved even when a later timeout/overflow occurs.
        nested_corrupt = False
        if marker and marker.startswith('FOREGROUND_CASE_OK '):
            expected=self.nested[case]['planned']; observed=self.nested[case]['observed']
            # On a product assertion, unvisited leaves are explicit gaps. A
            # duplicate/out-of-order/foreign marker is infrastructure corruption.
            nested_corrupt = observed != expected[:len(observed)]
        self.infrastructure_broken = timed or len(raw)>1024*1024 or nested_corrupt
        if behavioral and sanitizer:
            raise Contract(sanitizer[0], sanitizer[1][:240])
        if behavioral and (assertion or legacy):
            loc=re.search(r'([^\s,]+\.c)(?::|, line )(\d+)',text)
            fallback=str(path)+':'+str(text[:legacy.start()].count('\n')+1) if legacy else str(path)+':1'
            location=assertion[1] if assertion else (loc[1]+':'+loc[2] if loc else fallback)
            raise Contract(location,(assertion[2] if assertion else legacy[0])[:240])
        if nested_corrupt: raise Infrastructure('corrupt nested configuration marker sequence')
        if timed: raise Infrastructure('phase deadline; process group killed; partial log retained')
        if len(raw)>1024*1024: raise Infrastructure('bounded digest limit exceeded; full log retained')
        if rc and re.search(r"(?:region [`']FLASH['`].*overflowed|will not fit in region [`']FLASH)", text):
            raise Contract(str(path)+':1', 'real FLASH link overflow; boot 32KiB/slot limit unchanged; see raw log')
        if rc: raise Infrastructure(f'exit {rc} without trustworthy product assertion; {path}')
        if marker is not None and text.splitlines().count(marker)!=1:
            raise Infrastructure('missing/duplicate terminal marker: '+marker)
        if marker and marker.startswith('FOREGROUND_CASE_OK '):
            if self.nested[case]['observed']!=self.nested[case]['planned']:
                raise Infrastructure('missing/duplicate/out-of-order nested configuration markers: '+case)
        return text
    def check(self, condition, message, at='static:1'):
        if not condition: raise Contract(at,message)

    def main_order(self):
        path=self.args.root/'examples/ethernet-ftp/main.c'
        source=path.read_text()
        # Exact source admission/order oracle; NOT execution of main.
        pattern=r'#if defined\(LM3S6965_FTP_FOREGROUND_OWNER\)\n(.*?)#endif'
        selected=re.sub(pattern,lambda m:m[1].split('#else',1)[0],source,flags=re.S)
        selected=re.sub(r'/\*.*?\*/|//[^\n]*','',selected,flags=re.S)
        # Select actual main body; every required call occurs exactly once.
        body = selected.split('int main(void)', 1)[1]
        calls=['ram_file_init(', 'ftp_core_init(', 'ftp_core_reset(',
               'boot_startup_c_ready(', 'net_clock_initialize(', 'lwip_init(',
               'lm3s_netif_initialize(', 'boot_startup_gptm_release(', 'foreground_owner_turn(']
        positions=[body.find(call) for call in calls]
        self.check(all(body.count(call)==1 for call in calls) and
                   all(p>=0 for p in positions) and positions==sorted(set(positions)),
                   'actual main must order RAM/file+core init/reset<c_ready<netclock<lwIP/netif<release<owner',str(path)+':1')
        loop = body.find('for (;;)', positions[-2])
        self.check(positions[-2] < loop < positions[-1], 'release must precede owner loop',str(path)+':1')
        self.check('system_control_configure_pll(' not in selected,
                   'foreground main selects standalone PLL path',str(path)+':1')
        (self.directory/'main-order.json').write_text(json.dumps({'kind':'static-source-only',
            'calls':calls,'positions':positions,'runtime_main':False},indent=2)+'\n')

    def production_static(self, slot, config, build):
        details={}
        cc=json.loads((build/'compile_commands.json').read_text())
        foreground=[row for row in cc if 'foreground_platform.dir' in row.get('command','')]
        files={Path(row['file']).name for row in foreground}
        self.check({'net_clock.c','net_clock_port.c'} <= files,
                   'foreground compile roster must use actual clock and production port')
        self.check(not any('clock_switch_test' in row['file'] for row in cc),
                   'production compilation contains native fixture')
        for kind in ('foreground','standalone'):
            name='lm3s6965_ethernet_ftp'+('_foreground' if kind=='foreground' else '')
            elf=build/'examples/ethernet-ftp'/name
            text=self.command(['arm-none-eabi-nm','-n',elf]); sy={}
            for line in text.splitlines():
                fields=line.split()
                if len(fields)==3 and re.fullmatch('[0-9a-fA-F]+',fields[0]): sy[fields[2]]=int(fields[0],16)
            self.check(not self.command(['arm-none-eabi-nm','-u',elf]).strip(),'ELF has unresolved symbols')
            headers=self.command(['arm-none-eabi-readelf','-h','-l','-S','-W',elf])
            dis=self.command(['arm-none-eabi-objdump','-d',elf])
            size=self.command(['arm-none-eabi-size',elf])
            vec=self.directory/f'{slot}-{config}-{kind}-vectors.bin'
            self.command(['arm-none-eabi-objcopy','--dump-section','.isr_vector='+str(vec),elf])
            raw=vec.read_bytes()
            if len(raw)<8: raise Infrastructure('truncated vector dump')
            msp,reset=struct.unpack('<II',raw[:8])
            self.check(msp==0x20010000,'MSP differs from reserved stack top')
            entry='Application_Reset_Handler' if kind=='foreground' else 'Reset_Handler'
            self.check(reset==(sy[entry]|1),'reset vector does not name linked entry')
            self.check(sy['_diagnostic_start']==0x2000dfc0 and sy['_diagnostic_end']==0x2000e000
                       and sy['_stack_limit']==0x2000e000,'diagnostic/stack bounds differ')
            if kind=='foreground':
                origin=0x8800 if slot=='A' else 0x24000
                linker=(self.args.root/f'linker/app_{slot.lower()}.ld').read_text()
                authority=re.search(r'FLASH\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*(0x[0-9a-fA-F]+)',linker)
                self.check(authority is not None and int(authority[1],16)==origin,
                           'slot origin differs from linker authority')
                self.check(sy['_vectors_start']==origin,'foreground vectors at wrong slot origin')
                self.check(sy['boot_startup_continuity']==0x2000dbc0 and sy['_continuity_end']==0x2000dfc0
                           and sy['_heap_end']==0x2000dbc0,'retained/heap reservation differs')
                self.check(sy['_flash_payload_end']<=sy['_flash_payload_limit'],'foreground payload exceeds slot')
                self.check(sy['_edata']<=0x2000dbc0 and sy['_ebss']<=0x2000dbc0,'ordinary data overlaps continuity')
                self.check(all(n in sy for n in ('foreground_owner_turn','boot_startup_time',
                    'boot_startup_gptm_release','SysTick_Handler','net_clock_port_unmask')),
                    'missing handoff/owner/application tick symbols')
                self.check('system_control_configure_pll' not in sy,'foreground links PLL reinit')
                for section in ('boot_continuity','boot_diagnostic','stack'):
                    self.check(re.search(r'\.'+section+r'\s+NOBITS',headers) is not None,'missing NOLOAD '+section)
            else:
                self.check(not any(n.startswith(('boot_startup','foreground_owner','Application_')) for n in sy),
                           'standalone contains foreground continuity symbols')
                self.check('system_control_configure_pll' in sy and 'SysTick_Handler' in sy,
                           'standalone missing PLL/tick')
                self.check('<net_clock_initialize>:' in dis,'standalone clock function absent')
                clock=dis.split('<net_clock_initialize>:',1)[1].split('\n\n',1)[0]
                self.check('str' in clock,'standalone clock has no static store instruction')
                self.check(sy['_heap_end']-sy['_heap_start']==8192,'standalone heap reserve changed')
            details[kind]={'elf':str(elf),'sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),
                           'msp':hex(msp),'reset':hex(reset),'size':size.strip()}
        (self.directory/f'{slot}-{config}-static.json').write_text(json.dumps(details,indent=2)+'\n')

def main():
    invocation_started=float(os.environ.get('CLOCK_SWITCH_INVOCATION_STARTED', time.monotonic()))
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True)
    p.add_argument('--session',type=Path,required=True)
    p.add_argument('--phase',choices=ROSTERS,required=True)
    choice=p.add_mutually_exclusive_group(required=True)
    choice.add_argument('--all',action='store_true'); choice.add_argument('--case',action='append')
    p.add_argument('--arm-bin',type=Path,required=True)
    p.add_argument('--human',action='store_true'); p.add_argument('--failures-only',action='store_true')
    args=p.parse_args(); args.root=args.root.resolve(); args.session=args.session.resolve()
    args.invocation_started=invocation_started
    if Path('/tmp').resolve() not in args.session.parents: p.error('--session must be under /tmp')
    roster=ROSTERS[args.phase] if args.all else args.case
    if not roster or len(set(roster))!=len(roster) or any(c not in ROSTERS[args.phase] for c in roster):
        p.error('unknown/duplicate/empty case selection')
    if args.session != Path('/tmp/clock-switch-bootOs-session').resolve():
        p.error('use the new /tmp/clock-switch-bootOs-session only; never prior session state')
    if args.phase == 'first' and args.session.exists():
        p.error('first invocation requires a fresh nonexistent session; no replay')
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    args.session.mkdir(parents=True,exist_ok=True)
    rawbase=args.session/'artifacts'; rawbase.mkdir(exist_ok=True)
    directory=rawbase/(stamp+'-clock-switch-'+args.phase); directory.mkdir()
    # Raw JSON/bin/ELF/maps and build trees only in /tmp. Vault contains logs/digest only.
    base=args.root/'.dev-vault/tests/runs'; base.mkdir(parents=True,exist_ok=True)
    vault=base/directory.name; vault.mkdir()
    report=vault/'report.jsonl'; run=Run(args,directory)
    records=[]; stop=False; setup_error=None; initial=None; freeze_ok=False
    statepath=args.session/'state.json'; state={}; claimed=False
    index={'first': 1, 'second': 2, 'diagnostics': 3}[args.phase]
    try:
        identity=json.loads((AUTHOR/'identity.json').read_text())
        if hashlib.sha256((AUTHOR/'phase-driver.py').read_bytes()).hexdigest()!=identity['author_driver_sha256']:
            raise Infrastructure('author phase driver identity mismatch')
        frozen=Path('/tmp/clock-switch-gate-reviewed/source-freeze.json')
        if hashlib.sha256(frozen.read_bytes()).hexdigest()!=identity['historical_ledger_sha256']:
            raise Infrastructure('reviewed identity ledger changed')
        state=json.loads(statepath.read_text()) if statepath.exists() else {'attempted':[], 'passed':[]}
        if index == 3:
            if not state['attempted'] or 3 in state['attempted']:
                raise Infrastructure('diagnostics requires a prior attempt; never replay')
            if not any(x['status']=='invalid' for v in state.values() if isinstance(v,dict)
                       for x in v.get('observed',[])):
                raise Infrastructure('diagnostics conditional on retained infrastructure-invalid evidence')
        elif state['attempted']!=list(range(1,index)) or state['passed']!=list(range(1,index)):
            raise Infrastructure('prior invocation not fully passing or already consumed; no replay')
        reserved=sum(SECONDS[{1:'first',2:'second',3:'diagnostics'}[i]] for i in state['attempted'])
        if len(state['attempted'])>=3 or reserved+SECONDS[args.phase]>1420:
            raise Infrastructure('allocation exhausted: at most three invocations / 1420 seconds')
        state['reserved_seconds']=reserved+SECONDS[args.phase]
        state['attempted'].append(index); claimed=True
        statepath.write_text(json.dumps(state)+'\n')
        initial=freeze(args.root)
        (directory/'source-before.json').write_text(json.dumps(initial,sort_keys=True)+'\n')
        if initial!=identity['expected']: raise Infrastructure('identity differs from frozen ledger plus scoped overlay')
        # Reuse is per dependency, never inferred from the broad source overlay.
        for reuse in json.loads((AUTHOR/'reuse-ledger.json').read_text()):
            for name, expected in reuse['dependency_identities'].items():
                if initial.get(name) != expected:
                    raise Infrastructure('withdraw historical reuse on dependency mismatch: '+reuse['name']+' '+name)
    except Exception as exc: setup_error=str(exc); stop=True
    native=args.session/'focused'; signed=args.session/'signed'
    with report.open('w') as digest:
        for case in roster:
            try:
                if case=='source-freeze-after' and initial is not None:
                    after=freeze(args.root)
                    (directory/'source-after.json').write_text(json.dumps(after,sort_keys=True)+'\n')
                    if initial!=after: raise Infrastructure('source identity changed during invocation')
                    if time.monotonic()>run.deadline:
                        raise Infrastructure('phase allocation exhausted, including identity/report preparation')
                    freeze_ok=True
                elif stop: raise Infrastructure(setup_error or 'not attempted after prior non-pass; stopped')
                elif time.monotonic()>=run.deadline-5:
                    raise Infrastructure('phase allocation exhausted before case; finalization reserve retained')
                elif case=='harness-diagnostics':
                    # Read-only harness evidence review: no command/build/product replay.
                    evidence=[]
                    for phase in ('first','second'):
                        prior=state.get(phase)
                        if not prior: continue
                        raw=Path(prior['raw_artifacts']).resolve()
                        if args.session/'artifacts' not in raw.parents:
                            raise Infrastructure('prior raw evidence escaped owned /tmp artifacts')
                        data=json.loads((raw/'roster.json').read_text())
                        if data['observed'] != prior['observed'] or data['planned'] != prior['planned']:
                            raise Infrastructure('prior report/state disagreement; preserve originals')
                        evidence.append({'phase':phase, 'report':str(raw/'report.jsonl'),
                            'nonpass':[r for r in data['observed'] if r['status']!='pass']})
                    (directory/'harness-diagnostics.json').write_text(json.dumps({
                        'product_executions':0, 'repairs':0, 'prior_evidence':evidence})+'\n')
                elif case=='signed-bringup-build':
                    if not (args.root/'third_party/lwip/src/include/lwip/init.h').is_file():
                        raise Infrastructure('offline pinned lwIP absent')
                    run.command([sys.executable,args.root/'tests/boot/test_foreground_target.py',signed,'--build-only'],
                        behavioral=True,marker='FOREGROUND_BUILD_OK signed')
                elif case=='native-clock-build':
                    run.command(['cmake','-S',args.root/'tests/ethernet/foreground','-B',native,
                        '-DCMAKE_BUILD_TYPE=Debug','-DFOREGROUND_SANITIZERS=ON'])
                    run.command(['cmake','--build',native,'--target','foreground_clock',
                        'foreground_boot_startup','--parallel','4'])
                elif case in CLOCK:
                    run.command([native/'foreground_clock',case],True,'FOREGROUND_CASE_OK '+case)
                elif case=='boot_startup':
                    run.command([native/'foreground_boot_startup'],True,'STARTUP HOST PASS')
                elif case=='main_systick_gptm_network_release_order_static': run.main_order()
                elif case==SIGNED:
                    run.command([sys.executable,args.root/'tests/boot/test_foreground_target.py',signed,'--run-only'],
                        True,'FOREGROUND_CASE_OK '+SIGNED)
                    nested=json.loads((signed/'configurations.json').read_text())
                    expected=[{'configuration': c, 'status':'pass', 'scenarios':
                        [{'case':s,'status':'pass'} for s in
                         ('msp-clean','psp-clean','msp-masks','psp-masks','pre-c-clock-reject')]}
                        for c in ('bootOs/appOs-A','bootOs/appOg-B')]
                    if nested!=expected: raise Infrastructure('signed scenario/configuration artifact disagrees with markers')
                elif case.endswith(':build'):
                    slot,config=case.split(':')[0].split('-'); build=args.session/(slot+'-'+config)
                    run.command(['cmake','-S',args.root,'-B',build,
                        '-DCMAKE_TOOLCHAIN_FILE='+str(args.root/'cmake/toolchain-lm3s6965.cmake'),
                        '-DCMAKE_BUILD_TYPE='+config,'-DLM3S6965_BUILD_ETHERNET_FTP=ON',
                        '-DLM3S6965_FTP_FOREGROUND_OWNER=ON','-DLM3S6965_FTP_FOREGROUND_SLOT='+slot,
                        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DFETCHCONTENT_FULLY_DISCONNECTED=ON'])
                    run.command(['cmake','--build',build,'--target','lm3s6965_ethernet_ftp_foreground',
                        'lm3s6965_ethernet_ftp','--parallel','4'])
                elif case.endswith(':static'):
                    slot,config=case.split(':')[0].split('-')
                    run.production_static(slot,config,args.session/(slot+'-'+config))
                else: raise Infrastructure('unimplemented roster member')
                record={'case':case,'status':'pass'}
            except Contract as exc:
                record={'case':case,'status':'fail','at':exc.at,'assert':exc.message[:240]}
                stop=True
            except (Exception,KeyboardInterrupt) as exc:
                record={'case':case,'status':'invalid','reason':str(exc).replace('\n',' ')[:240] or type(exc).__name__}
                stop=True
            records.append(record); emit(record,digest)
            if not args.human and (not args.failures_only or record['status']!='pass'): emit(record,sys.stdout)
        if 'source-freeze-after' not in roster and initial is not None:
            try: freeze_ok=initial==freeze(args.root)
            except Exception: freeze_ok=False
        totals={s:sum(r['status']==s for r in records) for s in ('pass','fail','invalid')}
        complete=roster==ROSTERS[args.phase] and freeze_ok and not stop and not totals['invalid']
        passed=complete and not totals['fail']
        for record in records:
            if record['case'] in run.nested:
                nested=run.nested[record['case']]
                nested['case_status']=record['status']
                nested['unobserved']=[c for c in nested['planned'] if c not in nested['observed']]
                nested['unobserved_reason']='short-circuit-or-incomplete' if nested['unobserved'] else 'none'
        # Persist planned roster, including nested cases never reached.
        for case in roster:
            if case in CONFIGS and case not in run.nested:
                run.nested[case]={'planned':CONFIGS[case], 'observed':[], 'unobserved':CONFIGS[case],
                    'unobserved_reason':'not-attempted'}
        summary={'summary':{'target':'clock-switch-'+args.phase,'kind':'python',**totals,
                            'report':str(report),'log':str(vault/'run.log')}}
        rollup={'rollup':{**totals,'targets':1}}
        emit(summary,digest); emit(rollup,digest)
        if not args.human: emit(summary,sys.stdout); emit(rollup,sys.stdout)
    run.log.close(); shutil.copyfile(directory/'run.log',vault/'run.log')
    # Keep a full raw report and the reconciliation outside the vault.
    shutil.copyfile(report,directory/'report.jsonl')
    reconciliation={'planned':roster,'required':ROSTERS[args.phase],'observed':records,
        'execution_complete':complete,'passing':passed,'source_freeze_ok':freeze_ok,
        'elapsed_seconds':time.monotonic()-run.started,'allocation_seconds':SECONDS[args.phase],
        'nested_configurations':run.nested,'raw_artifacts':str(directory)}
    (directory/'roster.json').write_text(json.dumps(reconciliation,indent=2)+'\n')
    (vault/'evidence.md').write_text('Raw artifacts: `'+str(directory)+'`\n')
    if claimed:
        if passed: state['passed'].append(index)
        state[args.phase]=reconciliation; statepath.write_text(json.dumps(state,indent=2)+'\n')
    # No historical enumeration or deletion. This fresh owned session permits
    # one invocation per phase (three total), hence <=1 new run per phase/target.
    # Prior reports, failures, invalids and unrelated records are retained.
    if args.human:
        with (vault/'run.log').open() as stream: shutil.copyfileobj(stream,sys.stdout)
        print('Report: '+str(report))
    return 0 if passed else 1

if __name__=='__main__': sys.exit(main())
