#!/usr/bin/env python3
"""Opt-in immutable linked-handoff evidence. No build or target execution.

Four independent representatives; incomplete nested proof is INVALID. Historical
P8, configuration and checker outcomes are retained, never relabeled fresh.
"""
import argparse
from pathlib import Path
from checked_support import (ROOT, Session, Invalid, require, sha, source_identity,
    tool_identity, selfchecks, HANDOFF_CASES, NESTED, DISPATCH, COMMON, FORBIDDEN,
    AUTHORING_GAPS, handoff_roster, validate_handoff_migration, execute_handoff,
    handoff_reporting_selfchecks, handoff_migration_selfchecks, HandoffEvidence,
    HANDOFF_BINDINGS)
from checked_static import (static_selfchecks, inspect, retained, terminal_gate,
                            callbacks_gate, app_fixture)
from checked_entry import entry_gate, entry_selfchecks


def arguments():
    p=argparse.ArgumentParser(description=__doc__)
    selected=p.add_mutually_exclusive_group(required=True)
    selected.add_argument('--case',choices=HANDOFF_CASES,action='append')
    selected.add_argument('--all',action='store_true')
    p.add_argument('--bringup-only',action='store_true')
    p.add_argument('--no-build',action='store_true',required=True)
    p.add_argument('--reuse-report',type=Path,required=True)
    p.add_argument('--ftpa-elf',type=Path,required=True)
    p.add_argument('--obligation-roster',type=Path,required=True)
    migration=p.add_mutually_exclusive_group(required=True)
    migration.add_argument('--harness-migration',type=Path)
    migration.add_argument('--write-harness-migration',type=Path)
    p.add_argument('--migration-parent',type=Path)
    p.add_argument('--oracle-selfchecks',action='store_true')
    p.add_argument('--budget-seconds',type=int,choices=range(1,601),required=True)
    p.add_argument('--failures-only',action='store_true')
    p.add_argument('--human',action='store_true')
    a=p.parse_args()
    if a.case and len(a.case)!=len(set(a.case)): p.error('duplicate case')
    if a.write_harness_migration and not (a.bringup_only and a.oracle_selfchecks):
        p.error('migration creation requires bringup-only and oracle-selfchecks')
    if a.migration_parent and not a.write_harness_migration:
        p.error('migration-parent only accompanies a distinct repair sidecar')
    return a,[c for c in HANDOFF_CASES if a.all or c in (a.case or [])]


def main():
    a,selected=arguments()
    for case in HANDOFF_CASES: NESTED[case]=[]
    s=Session('checked-handoff-oracle',a,selected)
    evidence=HandoffEvidence(s)
    # Even a malformed/missing external roster leaves explicit unattempted
    # nested outcomes, never an empty roster that can accidentally pass.
    roster={'obligations':{c:[{'id':name,'expected':'INVALID: external expectation unavailable',
        'bound':'no evidence accepted without literal roster'}
        for name in (*HANDOFF_BINDINGS[c],*AUTHORING_GAPS[c])] for c in HANDOFF_CASES}}
    prior=None; images={}

    def migration(session,cache_file):
        nonlocal prior
        prior=validate_handoff_migration(session,cache_file,a.reuse_report,roster,
            sidecar=a.harness_migration,write=a.write_harness_migration,parent=a.migration_parent)

    def setup():
        roster.update(handoff_roster(a.obligation_roster))
        s.manifest['obligation_roster_sha256']=sha(a.obligation_roster)
        s.manifest['authoring_gaps']=AUTHORING_GAPS
        if a.oracle_selfchecks:
            evidence.support((('selfchecks',selfchecks),('entry_selfchecks',entry_selfchecks),
                ('static_selfchecks',static_selfchecks),
                ('handoff_reporting_selfchecks',handoff_reporting_selfchecks),
                ('handoff_migration_selfchecks',handoff_migration_selfchecks)))
        s.freeze(migration=migration)
        evidence.restore_support()
        s.manifest['historical_configurations']={
            'default':'historical OFF/OFF only','both-on':'historical exclusivity only',
            'physical-off':'historical trust rejection only'}
        s.manifest['limits']='Static category stimuli only; no fresh runtime/checker/configuration/hardware claims'
        # Bringup checks fixture availability/identity but grants no product pass.
        if a.bringup_only:
            for mode in ('dispatch','decision'):
                evidence.evaluate('fixture-'+mode,lambda mode=mode:fixture(mode))
            evidence.evaluate('fixture-ftpa',ftpa_identity)
            if any(r['status']!='pass' for r in evidence.gates.values()):
                raise Invalid('bringup fixture/selfcheck evidence incomplete')

    def ftpa_identity():
        from checked_support import FTPA_SHA
        require(not a.ftpa_elf.is_symlink() and a.ftpa_elf.is_file() and sha(a.ftpa_elf)==FTPA_SHA,
                'identified historical FTPA fixture')
        return {'path':str(a.ftpa_elf),'sha256':sha(a.ftpa_elf),'source_freshness_claim':False}

    def image(mode):
        if mode not in images:
            if prior is None: raise Invalid('immutable boot identity unavailable')
            target='lm3s6965_boot_dispatch' if mode=='dispatch' else 'lm3s6965_boot_reset'
            paths=[Path(p) for p in prior['artifacts'][target] if Path(p).name==target]
            if len(paths)!=1: raise Invalid('missing/ambiguous independently frozen ELF '+mode)
            images[mode]=inspect(s,paths[0])
        return images[mode]

    def fixture(mode):
        elf,dis=image(mode)
        return {'header':elf.header,'loads':elf.loads,'symbols':elf.symbols,
                'bindings':elf.bindings,'disassembly_bytes':len(dis)}

    def preservation():
        old,_=image('decision'); new,_=image('dispatch')
        for name in (*COMMON,'boot_sequence_run'): require(old.code(name),'decision typed closure '+name)
        for name in (*DISPATCH,*COMMON): require(new.code(name),'dispatch typed closure '+name)
        for name in FORBIDDEN: require(name not in new.symbols,'dispatch no forbidden leakage '+name)
        for name in DISPATCH: require(not old.code(name),'decision excludes executable dispatch '+name)
        for elf in (old,new):
            row=elf.symbol('boot_entry_state')
            require(row[1]==1 and row[2]=='.bss' and row[4]==4,'ordinary checkpoint object')
        for name in ('target','handoff','observed'):
            row=new.symbol(name)
            require(row[1]==1 and row[2]=='.bss' and row[3]%4==0 and
                    0x20000000<=row[3]<row[3]+row[4]<=0x2000dbc0,'ordinary aligned scratch '+name)
        resources={'decision':retained(old,0x8000),'dispatch':retained(new,0x8000)}
        s.manifest['static_resources']=resources
        return {'resources':resources,'decision_symbols':old.symbols,'dispatch_symbols':new.symbols,
                'loads':{'decision':old.loads,'dispatch':new.loads},
                'historical_configurations':s.manifest['migration']['configuration_records'],
                'immutable_build':s.manifest['migration']['build_records']}

    def dispatch_entry():
        elf,dis=image('dispatch'); return entry_gate(s,elf,dis,True)

    def decision_entry():
        elf,dis=image('decision'); return entry_gate(s,elf,dis,False)

    def callbacks():
        elf,dis=image('dispatch'); return callbacks_gate(elf,dis,s)

    def terminal():
        elf,dis=image('dispatch'); return terminal_gate(dis,elf)

    def witness(case):
        # evaluate catches each subgate separately: valid failures and remaining
        # independent decision/dispatch evidence survive subsequent invalidity.
        if case=='entry-inline-handoff':
            evidence.evaluate('preservation',preservation,case)
            evidence.evaluate('dispatch-entry',dispatch_entry,case)
            evidence.evaluate('decision-entry',decision_entry,case)
        elif case=='retained-callback-closure': evidence.evaluate('callbacks',callbacks,case)
        elif case=='terminal-transition': evidence.evaluate('terminal',terminal,case)
        elif case=='ftpa-initializer-native-abi': evidence.evaluate('ftpa',lambda:app_fixture(s),case)
        else: raise Invalid('unplanned witness '+case)

    def finalize():
        if hasattr(s,'identity'):
            require(s.identity=={'sources':source_identity(),'tools':tool_identity()},'completion source/tool identity')
            validate_handoff_migration(s,s.root/'checked-identity.json',a.reuse_report,roster,
                sidecar=a.write_harness_migration or a.harness_migration,parent=a.migration_parent)
        if any(AUTHORING_GAPS.values()):
            s.manifest['composite_blocker']='explicit required entry controls absent; no readiness claim'

    return execute_handoff(s,witness,setup,finalize,roster,evidence)


if __name__=='__main__': raise SystemExit(main())
