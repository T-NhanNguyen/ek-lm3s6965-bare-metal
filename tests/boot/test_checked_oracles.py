#!/usr/bin/env python3
"""Focused offline proof-helper regressions, not linked-image acceptance tests."""
import unittest
from unittest.mock import patch

import checked_entry as entry
import checked_static as static
from checked_support import Failure, Invalid


class DivisionPointerInvariant(unittest.TestCase):
    def rows(self, operands):
        return [
            (0x6f80, 'stmdb', 'sp!, {r4, r5, r6, r7, r8, r9, sl, lr}'),
            (0x6f8e, 'ldr', 'r2, [sp, #32]'),
            (0x7000, 'umull', operands),
            (0x702c, 'strd', 'r3, r4, [r2]'),
            (0x7030, 'ldmia', 'sp!, {r4, r5, r6, r7, r8, r9, sl, pc}'),
        ]

    def test_both_umull_destinations_preserve_result_pointer(self):
        for operands in ('r2, r3, r0, r1', 'r3, r2, r0, r1'):
            with self.subTest(operands=operands):
                with self.assertRaisesRegex(Failure, 'result pointer remains invariant'):
                    static.division_dependency(self.rows(operands))

    def test_umull_can_read_result_pointer(self):
        result = static.division_dependency(self.rows('r3, r4, r2, r1'))
        self.assertEqual(result['scope'], 'memory independence, not quotient correctness')
        self.assertEqual(result['memory'][-2], (0x702c, 'strd', 'r3, r4, [r2]'))


class EntryInstructionOracles(unittest.TestCase):
    def test_existing_and_instruction_selfchecks(self):
        entry.entry_selfchecks()

    def test_actual_semantics_corruptions_are_detected(self):
        # Mutate the shared interpreter implementation boundary, not a literal
        # expected result. Each positive selfcheck must now fail independently.
        real = entry.entry_memory_control
        for corruption, expected in (
            ('offset', 'interpreter field-offset expectation:'),
            ('copy', 'interpreter copy expectation:'),
            ('barrier', 'interpreter barrier-recording expectation:'),
            ('branch', 'interpreter branch expectation:'),
        ):
            def corrupt(pc, op, args, registers, flags, load, store, events, jump):
                if corruption == 'offset' and pc == 0x100 and op == 'ldr':
                    args = 'r2, [r0, #16]'
                elif corruption == 'copy' and pc == 0x120 and op == 'ldrb':
                    args = 'r6, [r4, #6]'
                elif corruption == 'barrier' and op == 'dmb':
                    return True  # Silently lose the interpreter's barrier event.
                elif corruption == 'branch' and op in ('beq', 'bne'):
                    op = 'bne' if op == 'beq' else 'beq'
                return real(pc, op, args, registers, flags, load, store, events, jump)

            with self.subTest(corruption=corruption):
                with patch.object(entry, 'entry_memory_control', corrupt):
                    with self.assertRaisesRegex(Invalid, 'oracle selfcheck: ' + expected):
                        entry.entry_selfchecks()

    def test_shared_addressing_keeps_writeback_and_pc_alignment(self):
        registers = {'r0': 0x1000}
        self.assertEqual(entry.entry_location(registers, '[r0, #4]!', 0), 0x1004)
        self.assertEqual(registers['r0'], 0x1004)
        self.assertEqual(entry.entry_location(registers, '[r0], #4', 0), 0x1004)
        self.assertEqual(registers['r0'], 0x1008)
        self.assertEqual(entry.entry_location(registers, '[pc, #8]', 0x102), 0x10c)


if __name__ == '__main__':
    unittest.main()
