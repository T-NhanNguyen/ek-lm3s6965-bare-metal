#!/usr/bin/env python3
"""Deterministic ELF-only checker regression; no Arm tools or target execution."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[2] / 'scripts/check-boot-reset-image.py'


def fixture(*, machine=40, entry=0x101, vector_address=0, vector_size=236,
            msp=0x20010000, irq=0x103, opcode=b'\x72\xb6',
            load_address=0, overrides=None):
    """Build just the ELF32 sections/symbols consumed by the production checker."""
    symbols = {'Boot_Reset_Handler': 0x101, 'Boot_Terminal_Handler': 0x103,
               '_flash_payload_end': 0x104, '_ebss': 0x20000000,
               '_heap_static_end': 0x20000100, '_heap_end': 0x2000dbc0,
               'calloc': 0x105, 'free': 0x107, '_sbrk': 0x109,
               'boot_verify_stored': 0x10b, 'boot_sequence_run': 0x10d}
    symbols.update(overrides or {})
    names = b'\0.shstrtab\0.isr_vector\0.text\0.strtab\0.symtab\0'
    strings = bytearray(b'\0')
    symtab = bytearray(16)  # Null symbol.
    for name, value in symbols.items():
        offset = len(strings)
        strings.extend(name.encode('ascii') + b'\0')
        symtab.extend(struct.pack('<IIIBBH', offset, value, 0, 0, 0, 0))
    words = [msp, 0x101] + [0 if i in {7, 8, 9, 10, 13} else 0x103
                            for i in range(2, 59)]
    words[58] = irq
    data = bytearray(0x200)
    struct.pack_into('<59I', data, 0x100, *words)
    data.extend(opcode + b'\0\0')
    sections = [(0,) * 10]
    for name, kind, address, payload, link, entsize in (
            ('.shstrtab', 3, 0, names, 0, 0),
            ('.isr_vector', 1, vector_address, b'', 0, 0),
            ('.text', 1, 0x100, b'', 0, 0),
            ('.strtab', 3, 0, strings, 0, 0),
            ('.symtab', 2, 0, symtab, 4, 16)):
        offset, size = len(data), len(payload)
        if name == '.isr_vector':
            offset, size = 0x100, vector_size
        elif name == '.text':
            offset, size = 0x200, 4
        sections.append((names.index(name.encode()), kind, 0, address,
                         offset, size, link, 0, 1, entsize))
        data.extend(payload)
    shoff = len(data)
    for section in sections:
        data.extend(struct.pack('<10I', *section))
    struct.pack_into('<16sHHIIIIIHHHHHH', data, 0,
                     b'\x7fELF\x01\x01\x01' + b'\0' * 9, 2, machine, 1,
                     entry, 52, shoff, 0, 52, 32, 1, 40, len(sections), 1)
    struct.pack_into('<8I', data, 52, 1, 0x100, 0, load_address, 236, 236, 5, 4)
    return bytes(data)


class BootResetImage(unittest.TestCase):
    def check_modes(self, data, error=None):
        with tempfile.TemporaryDirectory(prefix='boot-image-checker-') as tmp:
            elf = Path(tmp) / 'fixture.elf'
            elf.write_bytes(data)
            for mode in ('normal', '-O', 'PYTHONOPTIMIZE=1'):
                with self.subTest(mode=mode):
                    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
                    env.pop('PYTHONOPTIMIZE', None)
                    flags = ['-O'] if mode == '-O' else []
                    if mode == 'PYTHONOPTIMIZE=1':
                        env['PYTHONOPTIMIZE'] = '1'
                    result = subprocess.run(
                        [sys.executable, *flags, str(CHECKER), str(elf)],
                        env=env, capture_output=True, text=True, timeout=10)
                    if error is None:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn('closure checked', result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                        self.assertIn(error, result.stderr)
                        self.assertNotIn('closure checked', result.stdout)

    def test_valid_elf_in_all_modes(self):
        self.check_modes(fixture())

    def test_invalid_elf_in_all_modes(self):
        cases = [
            ('ident', b'BAD!' + fixture()[4:], 'requires ELF32 little-endian'),
            ('machine', fixture(machine=3), 'requires ARM'),
            ('entry', fixture(entry=0x100), 'Thumb ELF entry'),
            ('reset-thumb', fixture(overrides={'Boot_Reset_Handler': 0x100}), 'Thumb ELF entry'),
            ('terminal-thumb', fixture(overrides={'Boot_Terminal_Handler': 0x102}), 'Thumb ELF entry'),
            ('vector-address', fixture(vector_address=4), 'exact 59 vectors at flash zero'),
            ('vector-size', fixture(vector_size=232), 'exact 59 vectors at flash zero'),
            ('msp', fixture(msp=0x2000e000), 'MSP/reset vectors'),
            ('irq', fixture(irq=0), 'core and IRQ0..42 terminal mapping'),
            ('first-opcode', fixture(opcode=b'\x00\xbf'), 'cpsid i must be FIRST'),
            ('flash-end', fixture(overrides={'_flash_payload_end': 0x8001}), '32 KiB incl data LMA'),
            ('bss-order', fixture(overrides={'_ebss': 0x20000101}), 'BSS/static heap/heap bounds'),
            ('static-order', fixture(overrides={'_heap_static_end': 0x2000dbc1}), 'BSS/static heap/heap bounds'),
            ('heap-end', fixture(overrides={'_heap_end': 0x2000dbc4}), 'BSS/static heap/heap bounds'),
            ('load', fixture(load_address=0x8000), 'all load bytes in boot FLASH'),
        ]
        for name in ('Reset_Handler', 'main', '__libc_init_array'):
            cases.append((name, fixture(overrides={name: 0x111}),
                          f'application/constructor entry leaked: {name}'))
        for name in ('calloc', 'free', '_sbrk', 'boot_verify_stored', 'boot_sequence_run'):
            for value in (0, 0x8000):
                cases.append((f'{name}-{value}', fixture(overrides={name: value}),
                              f'missing/out-of-boot closure: {name}'))
        for name, data, error in cases:
            with self.subTest(case=name):
                self.check_modes(data, error)


if __name__ == '__main__':
    unittest.main()
