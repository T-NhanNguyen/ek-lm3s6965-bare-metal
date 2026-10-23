"""Offline signed release-boot + release/debug-app deployment continuity (not allDebug fit). Raw outputs must be in /tmp. No runtime main claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import traceback
from test_startup_target import build, static_gate, run, check_qemu_output

CONFIGS = [('bootOs/appOs-A', '-Os', 'A'), ('bootOs/appOg-B', '-Og', 'B')]
SCENARIOS = ['msp-clean', 'psp-clean', 'msp-masks', 'psp-masks', 'pre-c-clock-reject']

def main():
    p = argparse.ArgumentParser()
    p.add_argument('directory', type=Path)
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--run-only', action='store_true')
    args = p.parse_args()
    if args.build_only and args.run_only: p.error('choose at most one mode')
    directory = args.directory.resolve()
    if Path('/tmp').resolve() not in directory.parents: p.error('raw artifacts must be under /tmp')
    gcc = os.environ.get('BOOT_ARM_GCC') or shutil.which('arm-none-eabi-gcc')
    qemu = shutil.which('qemu-system-arm')
    if not gcc or (not args.build_only and not qemu):
        raise RuntimeError('FOREGROUND INFRASTRUCTURE: offline GNU Arm and QEMU required')
    directory.mkdir(parents=True, exist_ok=True)
    roster = [{'configuration': name, 'status': 'unexecuted',
               'scenarios': [{'case': case, 'status': 'unexecuted'} for case in SCENARIOS]}
              for name, _, _ in CONFIGS]
    report = directory / 'configurations.json'
    def save(): report.write_text(json.dumps(roster) + '\n')
    save()
    for index, (name, opt, slot) in enumerate(CONFIGS):
        target = directory / name.replace('/', '-')
        roster[index]['status'] = 'attempted'; save()
        if not args.run_only:
            boot, app, bd, ad = build(target, opt, slot, gcc, foreground=True)
            try:
                static_gate(boot, app, bd, ad, foreground=True)
                compilation = json.loads((target / 'optimization.json').read_text())
                for image in ('boot', 'app'):
                    rows = [row for row in compilation if row['image'] == image]
                    assert rows, 'missing compilation policy evidence: ' + image
                    for row in rows:
                        optimizations = [a for a in row['argv'] if a.startswith('-O')]
                        expected = '-Os' if image == 'boot' or row['source'].startswith('third_party/') else opt
                        assert optimizations and optimizations[-1] == expected, 'last optimization differs: ' + row['source']
            except AssertionError as exc:
                where=traceback.extract_tb(exc.__traceback__)[-1]
                print('FOREGROUND_ASSERT ' + where.filename + ':' + str(where.lineno) + ' ' + str(exc).replace('\n',' ')[:240], flush=True)
                raise
            identities={f:hashlib.sha256((target/f).read_bytes()).hexdigest() for f in
                        ('boot.elf','app.elf','app.bin','metadata.bin','app.package','optimization.json')}
            (target / 'signed-build-complete.json').write_text(json.dumps(
                {'configuration': name, 'sha256':identities}) + '\n')
        else:
            identity=json.loads((target / 'signed-build-complete.json').read_text())
            if identity.get('configuration') != name or set(identity.get('sha256',{})) != {
                    'boot.elf','app.elf','app.bin','metadata.bin','app.package','optimization.json'}:
                raise RuntimeError('FOREGROUND INFRASTRUCTURE: signed build identity absent')
            if any(hashlib.sha256((target/f).read_bytes()).hexdigest()!=h for f,h in identity['sha256'].items()):
                raise RuntimeError('FOREGROUND INFRASTRUCTURE: signed artifacts changed after bringup')
            boot = target / 'boot.elf'
        if args.build_only:
            roster[index]['status'] = 'built'; save(); continue
        for case, scenario in enumerate(SCENARIOS):
            roster[index]['scenarios'][case]['status'] = 'attempted'; save()
            try:
                output = run([qemu, '-M', 'lm3s6965evb', '-nographic', '-monitor', 'none',
                    '-serial', 'none', '-nic', 'none', '--semihosting-config',
                    'enable=on,target=native,arg=' + str(case), '-kernel', boot,
                    '-device', 'loader,file=' + str(target / 'app.bin') + ',addr=' +
                    ('0x8800' if slot == 'A' else '0x24000') + ',force-raw=on',
                    '-device', 'loader,file=' + str(target / 'metadata.bin') + ',addr=0x8000,force-raw=on'],
                    target, 'qemu-foreground-' + str(case), timeout=20)
                check_qemu_output(output, case)
            except BaseException:
                roster[index]['status'] = 'incomplete'; save(); raise
            roster[index]['scenarios'][case]['status'] = 'pass'; save()
            print('FOREGROUND_CONFIG_OK target_authenticated_foreground_continuity ' + name + '/' + scenario, flush=True)
        roster[index]['status'] = 'pass'; save()
    print('FOREGROUND_BUILD_OK signed' if args.build_only else
          'FOREGROUND_CASE_OK target_authenticated_foreground_continuity', flush=True)

if __name__ == '__main__': main()
