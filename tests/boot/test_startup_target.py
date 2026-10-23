"""Fresh independent boot -> signed A/B real C startup. Modeled WDT/time IO.
--build-only compiles/links/signs artifacts; never invokes QEMU or test checks.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import zlib

from test_layout import elf_tables
from test_handoff_target import instructions, check_sequence, require

ROOT = Path(__file__).resolve().parents[2]


# Bound retained evidence per stream; overflow is invalid, never a silent pass.
OUTPUT_LIMIT = 1024 * 1024


def raw_directory(directory):
    directory = Path(directory).resolve()
    if Path('/tmp').resolve() not in directory.parents:
        raise RuntimeError('STARTUP INFRASTRUCTURE: all raw writes must be under /tmp')
    return directory


def run(args, directory, label, timeout=120):
    directory = raw_directory(directory)
    argv = [str(x) for x in args]
    evidence = {'argv': argv, 'timeout_seconds': timeout, 'returncode': None,
                'timed_out': False, 'status': 'started'}
    metadata = directory / (label + '.json')
    metadata.write_text(json.dumps(evidence, indent=2) + '\n')
    with (directory / 'commands.log').open('a') as log:
        log.write(label + ' ' + repr(argv) + ' timeout=' + str(timeout) + '\n')
    error = None
    # Disk capture avoids unbounded PIPE buffers. subprocess.run kills/reaps its
    # direct child on timeout; file capture preserves byte-exact partial output.
    with (directory / (label + '.stdout')).open('w+b') as stdout, \
            (directory / (label + '.stderr')).open('w+b') as stderr:
        try:
            p = subprocess.run(argv, stdout=stdout, stderr=stderr, timeout=timeout)
            evidence.update(returncode=p.returncode,
                            status='passed' if p.returncode == 0 else 'command-failed')
        except subprocess.TimeoutExpired as exc:
            error = exc
            evidence.update(timed_out=True, status='infrastructure-timeout')
        except OSError as exc:
            error = exc
            evidence.update(status='infrastructure-launch-error', error=str(exc))
        streams = []
        for name, stream in (('stdout', stdout), ('stderr', stderr)):
            size = stream.tell()
            stream.seek(0)
            body = stream.read(OUTPUT_LIMIT)
            # Full raw stream already retained; bounded digest never overwrites it.
            evidence[name + '_bytes'] = size
            evidence[name + '_truncated'] = size > OUTPUT_LIMIT
            streams.append(body.decode('utf-8', errors='replace'))
    output = ''.join(streams)
    (directory / (label + '.log')).write_text(output)
    truncated = evidence['stdout_truncated'] or evidence['stderr_truncated']
    if truncated:
        evidence['output_incomplete'] = True
    metadata.write_text(json.dumps(evidence, indent=2) + '\n')
    with (directory / 'commands.log').open('a') as log:
        log.write(label + ' status=' + evidence['status'] + ' exit=' +
                  str(evidence['returncode']) + ' truncated=' + str(truncated) + '\n')
    if error is not None or truncated:
        raise RuntimeError('STARTUP INFRASTRUCTURE: ' + label + ' ' +
                           evidence['status'] + '; see ' + str(metadata)) from error
    if evidence['returncode']:
        raise RuntimeError(label + ': ' + output)
    return output


def check_qemu_output(output, case):
    # -nic none leaves the board's built-in NIC unconnected. Accept only this
    # exact single diagnostic prefix, never strip/filter raw command evidence.
    warning = 'qemu-system-arm: warning: nic stellaris_enet.0 has no peer\n'
    expected = 'STARTUP CLOCK REJECT PASS\n' if case == 4 else 'STARTUP PASS\n'
    require(output in (expected, warning + expected),
            'unexpected target output: ' + repr(output))


def build(directory, opt, slot, gcc, foreground=False):
    directory = raw_directory(directory)
    base = 0x8800 if slot == 'A' else 0x24000
    index = 0 if slot == 'A' else 1
    directory.mkdir(parents=True, exist_ok=True)
    flags = [gcc, '-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-fno-short-enums',
             opt, '-g', '-std=c11', '-ffunction-sections', '-fdata-sections', '-fstack-usage',
             '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'),
             '-I' + str(ROOT / 'include/lm3s6965'), '-I' + str(ROOT / 'tests/boot'),
             '-I' + str(directory), '-DSLOT_BASE=' + str(base), '-DSLOT_INDEX=' + str(index)]
    common = ['src/boot_startup.c', 'src/boot_startup_target.c', 'src/boot_watchdog.c',
              'src/boot_snapshot.c', 'src/boot_store.c', 'src/boot_format.c',
              'src/boot_diag_publication.c', 'src/boot_diag.c', 'src/startup.c']
    compilation = []
    def link(name, sources, linker, extra=()):
        objects = []
        for i, source in enumerate(sources):
            obj = directory / (name + '-' + str(i) + '.o')
            # Pinned crypto library remains Release -Os in both startup modes.
            library_opt = ['-Os'] if source.startswith('third_party/') else []
            stop_oracle = ['-Dboot_startup_stop=boot_startup_internal_stop'] if name == 'app' and source == 'src/boot_startup_target.c' else []
            # Foreground deployment ONLY: release boot, independently optimized app.
            # Last optimization wins for EVERY boot TU, including assembly/crypto.
            boot_opt = ['-Os'] if foreground and name == 'boot' else []
            argv = [*flags, *extra, *library_opt, *stop_oracle, *boot_opt,
                    '-c', ROOT / source, '-o', obj]
            run(argv, directory, name + '-compile-' + str(i))
            compilation.append({'image': name, 'source': source, 'argv': list(map(str, argv))})
            (directory / 'optimization.json').write_text(json.dumps(compilation) + '\n')
            objects.append(obj)
        elf = directory / (name + '.elf')
        run([gcc, '-mcpu=cortex-m3', '-mthumb', '-fno-short-enums', '-nostartfiles',
             '-Wl,--gc-sections,-z,max-page-size=0x100', '-Wl,-Map=' + str(directory / (name + '.map')),
             '-T' + str(ROOT / linker), *objects, '-lc', '-lgcc', '-lnosys', '-o', elf], directory, name + '-link')
        objdump = run([gcc, '-print-prog-name=objdump'], directory, name + '-objdump-path').strip()
        disassembly = run([objdump, '-d', '-t', elf], directory, name + '-disassembly')
        size = run([gcc.replace('gcc', 'size'), elf], directory, name + '-size')
        print(str(elf) + '\n' + size, flush=True)
        return elf, disassembly
    # Foreground headers need firmware lwipopts.h and arch/{cc,sys_arch}.h
    # from examples/ethernet-ftp plus pinned lwIP public headers. No host shims.
    foreground_sources = ['examples/ethernet-ftp/foreground_owner.c',
                          'examples/ethernet-ftp/net_clock.c',
                          'examples/ethernet-ftp/net_clock_port.c'] if foreground else []
    foreground_flags = (['-DFOREGROUND_CONTINUITY_TEST', '-DLM3S6965_FTP_FOREGROUND_OWNER',
                         '-I' + str(ROOT / 'examples/ethernet-ftp'),
                         '-I' + str(ROOT / 'third_party/lwip/src/include')]
                        if foreground else [])
    app, app_dis = link('app', [*common, 'src/boot_startup_entry.S',
                              'tests/boot/startup_app_fixture.c', *foreground_sources,
                              *(['src/boot_startup_gptm.c'] if foreground else [])],
                        'linker/app_' + slot.lower() + '.ld',
                        ['-DLM3S6965_APPLICATION_CONTINUITY', *foreground_flags])
    spec = importlib.util.spec_from_file_location('startup_package', ROOT / 'tools/boot/package.py')
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    key = Path(os.environ.get('BOOT_DEMO_KEY', str(Path.home() / '.local/share/lm3s6965-boot-demo' / ROOT.name / 'signing-key.pem')))
    payload = package.canonical_payload(app.read_bytes(), slot, 'elf')
    signed = package.build_package(payload, slot, 17, package.validate_key(key))
    (directory / 'app.bin').write_bytes(payload)
    (directory / 'app.package').write_bytes(signed)
    header = struct.pack('<4s7I', b'ABM1', 1, 168, 1, index, 0xffffffff, 0xffffffff, 1 << index)
    entry = hashlib.sha256(payload).digest() + struct.pack('<4I', 17, len(payload), 0, 0)
    metadata = header + (entry + bytes(48) if index == 0 else bytes(48) + entry)
    metadata += struct.pack('<4I', 0, 0xffffffff, 0, 0) + bytes(16)
    metadata += struct.pack('<2I', zlib.crc32(metadata), 0)
    (directory / 'metadata.bin').write_bytes(metadata + b'\xff' * (2048 - len(metadata)))
    _, symbols, _ = elf_tables(app.read_bytes())
    def array(name, data):
        return 'static const uint8_t ' + name + '[] = {' + ','.join(str(b) for b in data) + '};\n'
    (directory / 'startup_image.h').write_text(
        array('manifest_wire', signed[:160]) +
        '#define EXPECTED_LENGTH ' + str(len(payload)) + 'u\n' +
        '#define APP_DATA_START ' + str(symbols['_sdata']) + 'u\n' +
        '#define APP_BSS_END ' + str(symbols['_ebss']) + 'u\n')
    crypto = ['third_party/mbedtls/library/' + name + '.c' for name in
              ('asn1parse', 'asn1write', 'bignum', 'bignum_core', 'constant_time',
               'ecdsa', 'ecp', 'ecp_curves', 'platform_util', 'sha256')]
    boot, boot_dis = link('boot', [*common, 'src/boot_startup_terminal.S', 'src/boot_handoff.c',
                                  'src/boot_verify.c',
                                  'src/syscalls.c', 'tests/boot/startup_boot_fixture.c',
                                  'tests/boot/startup_dispatch_fixture.S', *crypto,
                                  *(['src/boot_startup_gptm.c'] if foreground else [])],
                          'linker/bootloader.ld', ['-I' + str(ROOT / 'config'),
                          '-I' + str(ROOT / 'third_party/mbedtls/include'),
                          '-DMBEDTLS_CONFIG_FILE="mbedtls_boot.h"', *foreground_flags])
    return boot, app, boot_dis, app_dis


def static_gate(boot, app, boot_dis, app_dis, foreground=False):
    raw_directory(boot.parent)
    tail = instructions(boot_dis, 'boot_startup_terminal')
    switch = next(i for i, row in enumerate(tail) if row[1:] == ('msr', 'msp, r1'))
    check_sequence(tail[switch:], [('msr', 'msp, r1'), ('isb', 'sy'),
        ('msr', 'control, r3'), ('isb', 'sy'), ('msr', 'basepri, r3'),
        ('msr', 'faultmask, r3'), ('msr', 'primask, r3'), ('bx', 'r2')], 'no-old-frame tail')
    prefix = instructions(app_dis, 'Application_Reset_Handler')
    expected = [('ldr', r'r0, \[pc, #[0-9]+\]')]
    for offset, register in enumerate(('msp', 'control', 'ipsr', 'primask', 'basepri', 'faultmask')):
        expected += [('mrs', 'r1, ' + register), ('str', r'r1, \[r0, #' + str(offset * 4) + r'\]')]
    expected += [('str', r'lr, \[r0, #24\]'), ('ldr', r'r1, \[pc, #[0-9]+\]'),
                 ('ldr', r'r1, \[r1, #0\]'), ('str', r'r1, \[r0, #28\]')]
    check_sequence(prefix, expected, 'pre-C capture exact allowlist')
    witness = instructions(app_dis, 'Application_Entry_Witness')
    first_call = next(i for i, row in enumerate(witness) if row[1] == 'bl')
    check_sequence(witness[:first_call], [('subw', 'r1, pc, #4'),
        ('str', r'r1, \[r0, #32\]'), ('ldr', r'r1, \[pc, #[0-9]+\]'),
        ('str', r'r1, \[r0, #36\]'), ('cpsid', 'i'),
        ('ldr', r'r0, \[pc, #[0-9]+\]'), ('ldr', r'r1, \[pc, #[0-9]+\]'),
        ('ldr', r'r2, \[pc, #[0-9]+\]')], 'witness before first new-stack call')
    callbacks = ('read_register', 'write_register', 'gptm_register_read', 'gptm_register_write', 'gptm_gate_delay') if foreground else ('read_register', 'write_register', 'read_clock')
    for name in callbacks:
        rows = instructions(boot_dis, name)
        require(not any(op in ('bl', 'blx') for _, op, _ in rows), 'callback must be closed leaf: ' + name)
        require(not any(op == '.word' and re.fullmatch(r'0x200[0-9a-f]{5}', operands)
                        for _, op, operands in rows), 'callback may not borrow ordinary SRAM: ' + name)
    for elf in (boot, app):
        sections, symbols, loads = elf_tables(elf.read_bytes())
        require(symbols['boot_startup_continuity'] == 0x2000dbc0, 'capsule address')
        require(symbols['_heap_end'] == 0x2000dbc0, 'heap clip')
        require(sections['.boot_continuity'][1:2] == (8,), 'NOLOAD continuity')
        require(sections['.boot_continuity'][5] == 1024, 'exact capsule span')
        require(symbols['_estack'] == 0x20010000, 'MSP unchanged')
        require(all(not p[4] or p[3] + p[4] <= (0x8000 if elf == boot else
                    symbols['_flash_payload_limit']) for p in loads), 'payload fit')
    _, bs, _ = elf_tables(boot.read_bytes())
    for name in (*callbacks, 'boot_startup_gptm_read', 'boot_startup_return_trap') if foreground else (*callbacks, 'boot_startup_return_trap'):
        require(bs[name] < 0x8000, 'boot resident closure: ' + name)
    if foreground:
        blocks = {m[1]: m[2] for m in re.finditer(
            r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=^[0-9a-f]+ <|\Z)', boot_dis, re.M | re.S)}
        pending = list(callbacks) + ['boot_startup_gptm_read']
        visited = set()
        while pending:
            name = pending.pop()
            if name in visited: continue
            visited.add(name)
            require(name in blocks and name in bs and bs[name] < 0x8000, 'boot closure symbol: ' + name)
            body = blocks[name]
            require(not re.search(r'\.word\s+0x200[0-9a-f]{5}', body), 'closure borrows ordinary SRAM: ' + name)
            for target in re.findall(r'\b(?:bl|blx|b(?:\.w)?)\s+[0-9a-f]+ <([^>]+)>', body):
                pending.append(target.split('+')[0])
        (boot.parent / 'boot-callback-closure.json').write_text(json.dumps(sorted(visited)) + '\n')
    _, aps, _ = elf_tables(app.read_bytes())
    require(aps['boot_startup_running'] >= aps['_flash_payload_start'], 'app running wrapper location')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    tools = sorted((Path.home() / '.toolchains').glob('*/bin/arm-none-eabi-gcc'))
    gcc = os.environ.get('BOOT_ARM_GCC') or (str(tools[-1]) if tools else shutil.which('arm-none-eabi-gcc'))
    if not gcc:
        raise RuntimeError('GNU ARM required, no skip')
    out = raw_directory(args.output) if args.output else Path(tempfile.mkdtemp(prefix='application-startup-', dir='/tmp'))
    out.mkdir(parents=True, exist_ok=True)
    print('Artifacts: ' + str(out), flush=True)
    for opt in ('-Os', '-Og'):
        for slot in ('A', 'B'):
            directory = out / (opt[1:] + '-' + slot)
            boot, app, bd, ad = build(directory, opt, slot, gcc)
            if args.build_only:
                continue
            static_gate(boot, app, bd, ad)
            qemu = shutil.which('qemu-system-arm')
            if not qemu:
                raise RuntimeError('QEMU required, no skip')
            for case in range(5):
                result = run([qemu, '-M', 'lm3s6965evb', '-nographic', '-monitor', 'none',
                    '-serial', 'none', '-nic', 'none', '--semihosting-config', 'enable=on,target=native,arg=' + str(case),
                    '-kernel', boot, '-device', 'loader,file=' + str(directory / 'app.bin') +
                    ',addr=' + ('0x8800' if slot == 'A' else '0x24000') + ',force-raw=on',
                    '-device', 'loader,file=' + str(directory / 'metadata.bin') + ',addr=0x8000,force-raw=on'],
                    directory, 'qemu-' + str(case), timeout=20)
                check_qemu_output(result, case)
            print(opt + ' ' + slot + ': real signed entry/data/BSS/constructor/retained callback PASS')


if __name__ == '__main__':
    main()
