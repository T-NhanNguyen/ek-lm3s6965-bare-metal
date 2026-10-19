#!/usr/bin/env python3
"""Offline public fingerprint publication regressions.

Six logical representatives; no builds, tool execution, network or target IO.
The AST anchor preserves the reviewed runner remainder; fresh physical-root
execution, not that structural assertion, supplies behavioral gate evidence.
"""
import argparse
import ast
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

ROOT = Path(__file__).resolve().parents[2]
# Independently transcribed OLD public selection; never import generator constants.
OLD_DIRS = ('src', 'include/lm3s6965', 'config', 'cmake',
            'third_party/mbedtls/include', 'third_party/mbedtls/library',
            'tests/boot/physical_root', 'freertos', 'examples')
OLD_SUFFIXES = ('.c', '.h', '.S', '.cmake', '.txt', '.ld')
OLD_REQUIRED = ('CMakeLists.txt', 'include/CMakeLists.txt',
    'scripts/validate-boot-public-key.py', 'tests/boot/CMakeLists.txt',
    'tests/boot/run_physical_root.py', 'tests/boot/physical_root_manifest.json',
    'tests/boot/run_sequence.py', 'tests/boot/sequence_host.c',
    'tests/boot/flash_model.c', 'tests/boot/flash_model.h',
    'tests/boot/verify_fixtures.h', 'tests/boot/test_crypto.c')
SEQUENCE_DIRS = ('include/lm3s6965', 'config', 'third_party/mbedtls/include',
                 'third_party/mbedtls/library')
SEQUENCE_REQUIRED = ('src/CMakeLists.txt', 'tests/boot/CMakeLists.txt',
    'cmake/boot_crypto.cmake', 'tests/boot/sequence_host.c', 'tests/boot/run_sequence.py',
    'tests/boot/sequence_manifest.json', 'tests/boot/flash_model.c',
    'tests/boot/flash_model.h', 'tests/boot/verify_fixtures.h', 'docs/boot-startup.md')
PROFILES = ('physical-root', 'sequence')
SEQUENCE_AST = '696143d8a088b42465cf1331e3e473b85bd157e7076a26979acf71ee1b09fcab'
GENERATOR = 'scripts/boot_fingerprints.py'
UNCHANGED_AST = '71d51ca37cd26e60745665041fc0137be819884b4e1ff82c9a5a21678388b7b4'
CASES = ('default-cwd', 'public-export', 'source-transition',
         'explicit-tools', 'output-privacy', 'delegation-navigation')
PLANS = {
 'default-cwd': ('Anchor both profiles to the script; preserve independent exact hashes and physical default.',
                 'default source-only invocation'),
 'public-export': ('Reproduce both independent public profile maps with empty PATH and scope-distinguishing decoys.',
                   'clean public checkout without private dependencies'),
 'source-transition': ('For each profile reject missing/aliased inputs and expose exactly one source digest transition.',
                       'source admission and byte identity'),
 'explicit-tools': ('Compose both profile maps with exact nonexecuted tool identities; reject invalid optional requests.',
                    'explicit optional identity admission'),
 'output-privacy': ('Reject both profiles input overwrites and future selected outputs; preserve bytes and aliases.',
                    'publication side-effect exclusion'),
 'delegation-navigation': ('Preserve both runner remainders/rosters while delegating exact profiles; retain clock navigation.',
                         'harness preservation and public navigation'),
}
class Invalid(Exception): pass
class Failure(Exception):
    def __init__(self, message):
        super().__init__(message)
        self.at = 'tests/boot/run_publication.py:' + str(inspect.currentframe().f_back.f_lineno)
def check(ok, message):
    if not ok: raise Failure(message)
def digest(data): return hashlib.sha256(data).hexdigest()
def parse_manifest(raw):
    def unique(pairs):
        result = {}
        for k, v in pairs:
            if k in result: raise ValueError('duplicate JSON key')
            result[k] = v
        return result
    return json.loads(raw, object_pairs_hook=unique)
def normalized_runner(tree):
    for n in ast.walk(tree):
        if isinstance(n, ast.FunctionDef) and n.name == 'identity': n.body = [ast.Pass()]
    tree.body = [n for n in tree.body if not (
        isinstance(n, ast.Import) and any(x.name == 'sys' for x in n.names)) and not (
        isinstance(n, ast.ImportFrom) and n.module == 'scripts.boot_fingerprints') and not (
        isinstance(n, ast.Expr) and isinstance(n.value, ast.Call) and
        ast.unparse(n.value.func) == 'sys.path.insert')]
    return digest(ast.dump(tree, include_attributes=False).encode())
def public_names(root, profile='physical-root'):
    names = set(OLD_REQUIRED if profile == 'physical-root' else SEQUENCE_REQUIRED) | {GENERATOR}
    if profile == 'sequence':
        names.update(p.relative_to(root).as_posix() for pattern in ('boot_*.c', 'boot_*.h')
                     for p in (root/'src').glob(pattern) if p.is_file())
    for directory in (OLD_DIRS if profile == 'physical-root' else SEQUENCE_DIRS):
        base = root / directory
        if not base.exists(): continue
        if base.is_symlink(): raise Invalid('fixture public directory alias')
        for parent, dirs, files in os.walk(base, followlinks=False):
            for name in dirs + files:
                p = Path(parent) / name
                if p.is_symlink(): raise Invalid('fixture contains symlink: ' + str(p))
                if name in ('.dev-vault', '.git', '.pi', 'privatekeys', 'private-keys',
                            'resume.tex', '.env') or name.startswith('.env.'):
                    raise Invalid('private entry encountered; no traversal/copy permitted')
            names.update((Path(parent) / name).relative_to(root).as_posix()
                         for name in files if Path(name).suffix in
                         (OLD_SUFFIXES if profile == 'physical-root' else ('.c', '.h')))
    for name in names:
        if not (root / name).is_file(): raise Invalid('fixture required file missing: ' + name)
    return sorted(names)

class Cases:
    def __init__(self, work, log, deadline):
        self.work, self.log, self.deadline = work, log, deadline
        self.env = dict(os.environ, PATH='', PYTHONDONTWRITEBYTECODE='1')
        # No generator import in the writer or runner parent; CLI children only.
    def remaining(self):
        value = self.deadline - time.monotonic()
        if value <= 0: raise Invalid('wall budget exhausted')
        return value
    def cli(self, script, args=(), cwd=None):
        self.remaining()
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            cmd = [sys.executable, str(script), *map(str, args)]
            self.log.write(('\n$ ' + repr(cmd) + '\n').encode()); self.log.flush()
            try:
                p = subprocess.Popen(cmd, cwd=cwd or self.work, env=self.env,
                                     stdout=out, stderr=err, start_new_session=True)
                try: p.wait(timeout=self.remaining())
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL); p.wait()
                    raise Invalid('child timeout; process group killed')
            except OSError as e: raise Invalid(str(e)) from e
            finally:
                for label, stream in ((b'\nSTDOUT\n', out), (b'\nSTDERR\n', err)):
                    self.log.write(label); stream.seek(0); shutil.copyfileobj(stream, self.log)
                self.log.flush()
            # Raw bytes preserved before bounded parsing.
            out.seek(0); err.seek(0)
            stdout, stderr = out.read(16*1024*1024+1), err.read(65537)
            if len(stdout)>16*1024*1024 or len(stderr)>65536:
                raise Invalid('CLI evidence exceeds parser bound; raw retained')
            return p.returncode, stdout, stderr
    def good(self, script, args=(), cwd=None):
        rc, out, err = self.cli(script, args, cwd)
        check(rc == 0 and err == b'', 'CLI succeeds with no stderr')
        try: obj = parse_manifest(out)
        except (ValueError, UnicodeError) as e: raise Failure('invalid manifest JSON: '+str(e))
        check(out == (json.dumps(obj, indent=2, sort_keys=True)+'\n').encode(),
              'exact sorted indented newline-terminated JSON bytes')
        return obj, out
    def reject(self, script, args, reason):
        rc, out, err = self.cli(script, args)
        check(rc == 1 and out == b'' and err.startswith(b'Fingerprint generation failed: ')
              and reason.encode() in err, 'specific fail-closed CLI rejection: '+reason)
    def export(self, label, profile='physical-root'):
        root = self.work / label; root.mkdir()
        names = public_names(ROOT, profile)
        expected = {}
        for name in names:
            self.remaining()
            data = (ROOT/name).read_bytes(); expected[name] = digest(data)
            dest = root/name; dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data)
        return root, expected
    def default_cwd(self):
        elsewhere = self.work/'elsewhere'; elsewhere.mkdir()
        default, default_raw = self.good(ROOT/GENERATOR)
        for profile in PROFILES:
            expected = {n:digest((ROOT/n).read_bytes()) for n in public_names(ROOT, profile)}
            args = ['--profile', profile]
            a, raw = self.good(ROOT/GENERATOR, args)
            b, raw2 = self.good(ROOT/GENERATOR, args, cwd=elsewhere)
            check(a == b == {'source_fingerprints':expected} and raw == raw2,
                  'exact independent profile map and cwd-independent bytes: '+profile)
            if profile == 'physical-root':
                check((a, raw) == (default, default_raw), 'default equals explicit physical-root')
    def public_export(self):
        for profile in PROFILES:
            root, expected = self.export('export-'+profile, profile)
            check(not (root/'.dev-vault').exists(), 'export contains no vault')
            docs = sorted(p.relative_to(root).as_posix() for p in (root/'docs').rglob('*') if p.is_file())
            check(docs == ([] if profile == 'physical-root' else ['docs/boot-startup.md']),
                  'export includes only profile-required documentation')
            args = ['--profile', profile]
            actual, _ = self.good(root/GENERATOR, args)
            check(actual == {'source_fingerprints':expected}, 'clean export exact independent hash mapping')
            original, _ = self.good(ROOT/GENERATOR, args)
            check(actual == original, 'public export equals original CLI with empty PATH')
            # Public decoys distinguish recursive physical scope from direct boot-only sequence scope.
            for name in ('src/not_boot.c', 'src/nested/boot_nested.c', 'config/profile-note.txt'):
                p = root/name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'PUBLIC DECOY\n')
                if profile == 'physical-root': expected[name] = digest(p.read_bytes())
            check(self.good(root/GENERATOR, args)[0] == {'source_fingerprints':expected},
                  'exact profile selection with public decoys')
    def source_transition(self):
        root, expected = self.export('transition'); script = root/GENERATOR
        target = root/'tests/boot/flash_model.h'; saved = target.read_bytes()
        target.unlink()
        self.reject(script, [], 'required source file missing: tests/boot/flash_model.h')
        target.write_bytes(saved + b'\n/* publication fixture */\n')
        after = dict(expected); after['tests/boot/flash_model.h'] = digest(target.read_bytes())
        check(self.good(script)[0] == {'source_fingerprints':after}, 'exact single-source hash delta')
        target.unlink(); alias = root/'payload'; alias.write_bytes(saved); target.symlink_to(alias)
        self.reject(script, [], 'source symlink is not allowed:')
        check(alias.read_bytes() == saved and target.is_symlink(), 'source symlink and bytes preserved')
        target.unlink(); target.write_bytes(saved)
        d = root/'src'; parked = root/'src-parked'; d.rename(parked); d.symlink_to(parked, target_is_directory=True)
        self.reject(script, [], 'source directory symlink:')
        check(d.is_symlink(), 'directory alias remains untouched')
        root, expected = self.export('sequence-transition', 'sequence'); script = root/GENERATOR
        args = ['--profile', 'sequence']
        target = root/'docs/boot-startup.md'; saved = target.read_bytes(); target.unlink()
        self.reject(script, args, 'required source file missing: docs/boot-startup.md')
        target.write_bytes(saved+b'\nPUBLIC FIXTURE\n')
        after = dict(expected); after['docs/boot-startup.md'] = digest(target.read_bytes())
        check(self.good(script,args)[0] == {'source_fingerprints':after}, 'sequence exact one-source transition')
        target.unlink(); alias=root/'payload'; alias.write_bytes(saved); target.symlink_to(alias)
        self.reject(script,args,'source symlink is not allowed:')
        check(alias.read_bytes()==saved and target.is_symlink(), 'sequence source alias preserved')
        target.unlink(); target.write_bytes(saved)
        d=root/'src'; parked=root/'src-parked'; d.rename(parked); d.symlink_to(parked,target_is_directory=True)
        self.reject(script,args,'source directory symlink:')
        check(d.is_symlink(), 'sequence directory alias preserved')
    def explicit_tools(self):
        root, expected = self.export('tools-root'); script = root/GENERATOR
        bundle = self.work/'bundle'; bundle.mkdir(); marker = self.work/'EXECUTED'
        exe = bundle/'fake-cc'; exe.write_text('#!/bin/sh\nprintf executed > '+str(marker)+'\n'); exe.chmod(0o755)
        data = bundle/'lib.bin'; data.write_bytes(b'public library\x00')
        alias = bundle/'lib-alias'; alias.symlink_to(data.name)
        args = ['--tool', 'cc='+str(exe), '--tool-bundle', str(bundle)]
        expected_all = {'source_fingerprints':expected,
            'tools':{'cc':{'path':str(exe.resolve()),'sha256':digest(exe.read_bytes())}},
            'tool_bundles':{str(bundle.resolve()):{'fake-cc':digest(exe.read_bytes()),
                'lib-alias':digest(data.read_bytes()),'lib.bin':digest(data.read_bytes())}}}
        check(self.good(script,args)[0] == expected_all, 'exact tool and complete bundle tuples')
        check(not marker.exists() and alias.is_symlink(), 'tools never executed and symlink preserved')
        sequence, sequence_expected = self.export('sequence-tools-root', 'sequence')
        expected_sequence = dict(expected_all, source_fingerprints=sequence_expected)
        check(self.good(sequence/GENERATOR, ['--profile','sequence',*args])[0] == expected_sequence,
              'sequence exact optional tool/bundle composition')
        check(not marker.exists(), 'sequence tools never executed')
        self.reject(script, ['--tool','cc=not-installed'], 'tool not found or not executable:')
        data.chmod(0o644)
        self.reject(script, ['--tool','cc='+str(data)], 'tool not found or not executable:')
        self.reject(script, ['--tool','cc='+str(exe),'--tool','cc='+str(exe)], 'unique nonempty')
        empty = self.work/'empty'; empty.mkdir()
        self.reject(script, ['--tool-bundle',str(empty)], 'tool bundle is empty:')
        self.reject(script, ['--tool-bundle',str(self.work/'missing')], 'tool bundle missing:')
        self.reject(script, ['--tool-bundle',str(root)], 'bundle must not contain the repository:')
        outside = self.work/'outside'; outside.write_bytes(b'public outside')
        alias.unlink(); alias.symlink_to(outside)
        self.reject(script, ['--tool-bundle',str(bundle)], 'bundle symlink escapes bundle:')
        check(alias.is_symlink() and outside.read_bytes()==b'public outside', 'escaping alias unchanged')
        alias.unlink(); alias.symlink_to(data.name)
        link = bundle/'dir-alias'; link.symlink_to(empty, target_is_directory=True)
        self.reject(script, ['--tool-bundle',str(bundle)], 'directory symlink is not traversed:')
        check(link.is_symlink() and not marker.exists(), 'rejections do not execute or repair inputs')
    def output_privacy(self):
        root, expected = self.export('privacy-root'); script = root/GENERATOR
        source = root/'tests/boot/flash_model.h'; old = source.read_bytes()
        for output in (source, self.work/'hard-alias', self.work/'soft-alias'):
            if output.name=='hard-alias': os.link(source, output)
            if output.name=='soft-alias': output.symlink_to(source)
            self.reject(script, ['--output',str(output)], 'output would overwrite fingerprinted input:')
            check(source.read_bytes()==old, 'source bytes preserved on output rejection')
        check((self.work/'soft-alias').is_symlink() and
              (self.work/'hard-alias').stat().st_ino==source.stat().st_ino, 'output aliases preserved')
        future = root/'src/new-output.c'
        self.reject(script,['--output',str(future)], 'output would become an allowlisted source input')
        check(not future.exists(), 'rejected output not created')
        # Synthetic public sentinel under a forbidden basename, never real private material.
        forbidden = self.work/'.env.publication-fixture'; forbidden.write_bytes(b'SYNTHETIC SENTINEL')
        alias = self.work/'private-alias'; alias.symlink_to(forbidden)
        for output in (forbidden, alias):
            self.reject(script,['--output',str(output)], 'private input is forbidden:')
        self.reject(script,['--root',str(alias)], 'private input is forbidden:')
        self.reject(script,['--tool-bundle',str(alias)], 'private input is forbidden:')
        check(forbidden.read_bytes()==b'SYNTHETIC SENTINEL' and alias.is_symlink(), 'privacy rejection preserves sentinel and alias')
        bundle = self.work/'output-bundle'; bundle.mkdir(); exe=bundle/'cc'
        exe.write_bytes(b'NOT EXECUTED'); exe.chmod(0o755)
        self.reject(script,['--tool','cc='+str(exe),'--output',str(exe)], 'output would overwrite fingerprinted input:')
        self.reject(script,['--tool-bundle',str(bundle),'--output',str(bundle/'new.json')], 'output must be outside requested tool bundles')
        check(exe.read_bytes()==b'NOT EXECUTED' and not (bundle/'new.json').exists(), 'optional hashed inputs preserved')
        output = self.work/'manifest.json'
        rc, stdout, stderr = self.cli(script,['--output',str(output)])
        check((rc,stdout,stderr)==(0,b'',b''), 'successful file output has empty stdout/stderr')
        check(parse_manifest(output.read_bytes())=={'source_fingerprints':expected}, 'file output exact source mapping')
        root, expected = self.export('sequence-privacy-root', 'sequence'); script=root/GENERATOR
        args=['--profile','sequence']; source=root/'docs/boot-startup.md'; saved=source.read_bytes()
        for name in ('direct','hard','soft'):
            output=source if name=='direct' else self.work/('sequence-'+name)
            if name=='hard': os.link(source,output)
            if name=='soft': output.symlink_to(source)
            self.reject(script,[*args,'--output',str(output)],'output would overwrite fingerprinted input:')
            check(source.read_bytes()==saved, 'sequence required source preserved')
            if name=='hard': check(output.stat().st_ino==source.stat().st_ino, 'sequence hard alias preserved')
            if name=='soft': check(output.is_symlink(), 'sequence soft alias preserved')
        future=root/'src/boot_publication_fixture.c'
        self.reject(script,[*args,'--output',str(future)],'output would become an allowlisted source input')
        check(not future.exists(), 'sequence future source not created')
        output=root/'src/publication-output.c'
        check(self.cli(script,[*args,'--output',str(output)])==(0,b'',b''), 'nonselected sequence output allowed')
        check(parse_manifest(output.read_bytes())=={'source_fingerprints':expected}, 'sequence output exact mapping')
        check(self.good(script,args)[0]=={'source_fingerprints':expected}, 'nonselected output leaves source map unchanged')
    def delegation_navigation(self):
        path = ROOT/'tests/boot/run_physical_root.py'; text = path.read_text(); tree = ast.parse(text)
        identities = [n for n in ast.walk(tree) if isinstance(n,ast.FunctionDef) and n.name=='identity']
        check(len(identities)==1 and len(identities[0].body)==1 and
              ast.dump(identities[0].body[0],include_attributes=False)==
              ast.dump(ast.parse('return source_fingerprints(ROOT)').body[0],include_attributes=False),
              'nested identity delegates exactly to source_fingerprints(ROOT)')
        check(any(isinstance(n,ast.ImportFrom) and n.module=='scripts.boot_fingerprints' and
                  [(a.name,a.asname) for a in n.names]==[('source_fingerprints',None)] for n in tree.body),
              'public API imported directly')
        check(any(isinstance(n,ast.Expr) and isinstance(n.value,ast.Call) and
                  ast.unparse(n.value)=='sys.path.insert(0, str(ROOT))' for n in tree.body),
              'repository import path established')
        sys_imports=[i for i,n in enumerate(tree.body) if isinstance(n,ast.Import) and
                     [(x.name,x.asname) for x in n.names]==[('sys',None)]]
        path_setup=[i for i,n in enumerate(tree.body) if isinstance(n,ast.Expr) and
                    isinstance(n.value,ast.Call) and ast.unparse(n.value)=='sys.path.insert(0, str(ROOT))']
        api_imports=[i for i,n in enumerate(tree.body) if isinstance(n,ast.ImportFrom) and
                     n.module=='scripts.boot_fingerprints']
        check(len(sys_imports)==len(path_setup)==len(api_imports)==1 and
              sys_imports[0]<path_setup[0]<api_imports[0], 'sys import/path setup precede API import')
        check(normalized_runner(tree)==UNCHANGED_AST,
              'all non-integration AST unchanged: tool identity/version, baseline, before/after, assertions')
        sequence_text=(ROOT/'tests/boot/run_sequence.py').read_text(); sequence=ast.parse(sequence_text)
        identities=[n for n in ast.walk(sequence) if isinstance(n,ast.FunctionDef) and n.name=='identity']
        check(len(identities)==1 and len(identities[0].body)==1 and
              ast.dump(identities[0].body[0],include_attributes=False)==
              ast.dump(ast.parse("return source_fingerprints(ROOT, profile='sequence')").body[0],include_attributes=False),
              'sequence identity delegates exact profile')
        check(normalized_runner(sequence)==SEQUENCE_AST, 'sequence non-integration AST unchanged')
        sequence_names=next(ast.literal_eval(n.value) for n in sequence.body if isinstance(n,ast.Assign) and
                            any(isinstance(t,ast.Name) and t.id=='CASES' for t in n.targets))
        sequence_manifest=json.loads((ROOT/'tests/boot/sequence_manifest.json').read_text())
        check(tuple(c['name'] for c in sequence_manifest['cases'])==sequence_names, 'sequence12 roster unchanged')
        manifest = json.loads((ROOT/'tests/boot/physical_root_manifest.json').read_text())
        runner_cases = next(n.value for n in ast.parse(text).body if isinstance(n,ast.Assign) and
                            any(isinstance(t,ast.Name) and t.id=='CASES' for t in n.targets))
        check(tuple(x['name'] for x in manifest['cases'])==ast.literal_eval(runner_cases), 'physical11 roster remains exact')
        for rel in ('README.md','scripts/README.md','tests/README.md'):
            doc = ROOT/rel; body=doc.read_text()
            for dest in re.findall(r'\[[^\]]*\]\(([^\s)]+)\)', body):
                if '://' in dest or dest.startswith('#'): continue
                local = dest.split('#',1)[0]
                resolved = (doc.parent/local).resolve()
                check(resolved.is_relative_to(ROOT) and resolved.exists(), 'local documentation target exists: '+rel+' -> '+dest)
        top=(ROOT/'README.md').read_text(); index=(ROOT/'tests/README.md').read_text()
        check('(tests/README.md)' in top and '(scripts/README.md)' in top,
              'top-level feature and script navigation linked')
        check('## Current boot owner gates' in index and '## Focused historical diagnostics' in index and
              'clock_switch_test.c' in index and '(ethernet/foreground/runner.py)' in index,
              'current clock owner separated from historical diagnostics')
        cmake=(ROOT/'tests/ethernet/foreground/CMakeLists.txt').read_text()
        check('add_executable(foreground_clock clock_switch_test.c ' in cmake,
              'current registered clock target uses clock_switch_test.c')
        foreground=ast.parse((ROOT/'tests/ethernet/foreground/runner.py').read_text())
        cases=next(ast.literal_eval(n.value) for n in foreground.body if isinstance(n,ast.Assign) and
                   any(isinstance(t,ast.Name) and t.id=='CASES' for t in n.targets))
        check('systick_offset_no_retained_io' in cases['clock'], 'existing focused clock case remains selectable; not executed here')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    select=p.add_mutually_exclusive_group(required=True)
    select.add_argument('--all',action='store_true')
    select.add_argument('--case',action='append',choices=CASES)
    p.add_argument('--budget-seconds',type=int,required=True,choices=range(1,601))
    p.add_argument('--human',action='store_true'); p.add_argument('--failures-only',action='store_true')
    a=p.parse_args()
    if a.case and len(set(a.case))!=len(a.case): p.error('duplicate case')
    roster=[c for c in CASES if a.all or c in a.case]
    deadline=time.monotonic()+a.budget_seconds
    base=ROOT/'.dev-vault/tests/runs'; base.mkdir(parents=True,exist_ok=True)
    out=base/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-publication')
    out.mkdir(); reportpath=out/'report.jsonl'; logpath=out/'run.log'
    records=[]; stopped=None
    with reportpath.open('w') as report, logpath.open('wb') as log:
        def emit(r):
            line=json.dumps(r,separators=(',',':')); report.write(line+'\n'); report.flush()
            if not a.human and (not a.failures_only or r.get('status')!='pass'): print(line,flush=True)
        try:
            # Supporting oracle self-check, not a seventh logical representative.
            sample=b'{"source_fingerprints":{"a.c":"abc"},"tools":{"cc":{"path":"/public/cc","sha256":"def"}}}\n'
            obj=parse_manifest(sample)
            if (list(obj['source_fingerprints'].items()), list(obj['tools']['cc'].items())) != ([('a.c','abc')],[('path','/public/cc'),('sha256','def')]):
                raise Invalid('oracle exact tuple self-check failed')
            try: parse_manifest(b'{"a":1,"a":2}')
            except ValueError: pass
            else: raise Invalid('oracle accepted duplicate JSON key')
            if not (ROOT/GENERATOR).is_file(): raise Invalid('install proposal at tests/boot/run_publication.py before use')
        except Exception as e: stopped=str(e)
        for case in roster:
            record=None
            if stopped: record={'case':case,'status':'invalid','reason':'not attempted after infrastructure failure: '+stopped[:150]}
            else:
                try:
                    # Fresh case tree, no leaked fixture state. No imports of executable runner main.
                    with tempfile.TemporaryDirectory(prefix='publication-') as temp:
                        suite=Cases(Path(temp),log,deadline)
                        suite.remaining(); getattr(suite,case.replace('-','_'))(); suite.remaining()
                    record={'case':case,'status':'pass'}
                except Failure as e:
                    log.write(('FULL ASSERTION '+e.at+' '+str(e)+'\n').encode()); log.flush()
                    record={'case':case,'status':'fail','at':e.at,'assert':str(e).replace('\n',' ')[:240]}
                except Exception as e:
                    stopped=type(e).__name__+': '+str(e)
                    log.write(('INVALID '+stopped+'\n').encode()); log.flush()
                    record={'case':case,'status':'invalid','reason':stopped.replace('\n',' ')[:240]}
            records.append(record); emit(record)
        counts={s:sum(r['status']==s for r in records) for s in ('pass','fail','invalid')}
        (out/'manifest.json').write_text(json.dumps({'planned':roster,'observed':records,
            'missing':[c for c in roster if sum(r['case']==c for r in records)!=1],
            'complete':not stopped and len(records)==len(roster),
            'configuration':'Python stdlib; PATH empty; CLI only; no builds',
            'intents':{c:dict(zip(('intent','class'),PLANS[c])) for c in roster}},indent=2)+'\n')
        emit({'summary':{'target':'publication','kind':'python',**counts,'report':str(reportpath),'log':str(logpath)}})
        emit({'rollup':{**counts,'targets':1}})
    for old in sorted(base.glob('*-publication'))[:-5]:
        if old.is_dir() and not old.is_symlink(): shutil.rmtree(old)
    if a.human:
        with logpath.open() as log: shutil.copyfileobj(log,sys.stdout)
        with reportpath.open() as report: shutil.copyfileobj(report,sys.stdout)
    return int(bool(counts['fail'] or counts['invalid']))
if __name__=='__main__': raise SystemExit(main())
