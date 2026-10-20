#!/usr/bin/env python3
"""Exact boot-reset ELF/vector/entry checks; no target execution or ELF packages."""
import argparse
from pathlib import Path
import struct


def check(path):
    data = Path(path).read_bytes()
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('requires ELF32 little-endian')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if header[2] != 40:
        raise ValueError('requires ARM')
    entry, phoff, shoff = header[4:7]
    phsize, phnum, shsize, shnum, strindex = header[9:14]
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize)
                for i in range(shnum)]
    names_section = sections[strindex]
    names = data[names_section[4]:names_section[4] + names_section[5]]

    def string(table, offset):
        return table[offset:table.index(b'\0', offset)].decode('ascii')

    named = {string(names, s[0]): s for s in sections}
    symbols = {}
    for s in sections:
        if s[1] != 2:
            continue
        strings_section = sections[s[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for off in range(s[4], s[4] + s[5], s[9]):
            name, value, *_ = struct.unpack_from('<IIIBBH', data, off)
            symbols[string(strings, name)] = value
    reset = symbols['Boot_Reset_Handler']
    terminal = symbols['Boot_Terminal_Handler']
    if not (reset & 1 and terminal & 1 and entry == reset):
        raise ValueError('Thumb ELF entry')
    vector = named['.isr_vector']
    if not (vector[3] == 0 and vector[5] == 236):
        raise ValueError('exact 59 vectors at flash zero')
    words = struct.unpack_from('<59I', data, vector[4])
    if words[:2] != (0x20010000, reset):
        raise ValueError('MSP/reset vectors')
    reserved = {7, 8, 9, 10, 13}
    if not all(words[i] == (0 if i in reserved else terminal)
               for i in range(2, 59)):
        raise ValueError('core and IRQ0..42 terminal mapping')
    for name in ('Reset_Handler', 'main', '__libc_init_array'):
        if name in symbols:
            raise ValueError(f'application/constructor entry leaked: {name}')
    text = named['.text']
    offset = text[4] + (reset & ~1) - text[3]
    if data[offset:offset + 2] != b'\x72\xb6':
        raise ValueError('cpsid i must be FIRST')
    if symbols['_flash_payload_end'] > 0x8000:
        raise ValueError('32 KiB incl data LMA')
    if not (symbols['_ebss'] <= symbols['_heap_static_end'] <=
            symbols['_heap_end'] == 0x2000dbc0):
        raise ValueError('BSS/static heap/heap bounds must end at 0x2000dbc0')
    for i in range(phnum):
        segment = struct.unpack_from('<8I', data, phoff + i * phsize)
        kind, _, _, physical, filesz, *_ = segment
        if kind == 1 and filesz:
            if physical + filesz > 0x8000:
                raise ValueError('all load bytes in boot FLASH')
    for name in ('calloc', 'free', '_sbrk', 'boot_verify_stored', 'boot_sequence_run'):
        if not 0 < symbols[name] < 0x8000:
            raise ValueError(f'missing/out-of-boot closure: {name}')
    print('boot reset: exact vectors, Thumb entry, first cpsid, load/RAM bounds and closure checked')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf')
    check(parser.parse_args().elf)
