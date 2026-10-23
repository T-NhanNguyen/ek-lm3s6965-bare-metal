"""Offline compiler-path contracts; temporary executables are never executed."""
import ast
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from arm_compiler import arm_gcc


HERE = Path(__file__).resolve().parent
RUNNERS = ('checked_support.py', 'run_sequence.py', 'run_physical_root.py')
PINNED = '.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc'


class ArmCompilerTests(unittest.TestCase):
    def test_home_default_ignores_path(self):
        with patch.dict(os.environ, {'PATH': '/unrelated'}, clear=True), \
                patch('arm_compiler.Path.home', return_value=Path('/portable/home')):
            self.assertEqual(arm_gcc(), Path('/portable/home') / PINNED)
            for runner in RUNNERS:
                with self.subTest(runner=runner):
                    values = runpy.run_path(str(HERE / runner))
                    self.assertEqual(values['GCC'], Path('/portable/home') / PINNED)
                    self.assertEqual(values['ROOT'], HERE.parents[1])
                    self.assertEqual(values['CC'], Path('/usr/bin/clang'))

    def test_override_and_sibling_tools(self):
        with tempfile.TemporaryDirectory(prefix='arm tools ') as tmp:
            compiler = Path(tmp) / 'bin' / 'arm-none-eabi-gcc'
            compiler.parent.mkdir()
            compiler.write_text('NOT EXECUTABLE CONTENT\n')
            compiler.chmod(0o755)
            with patch.dict(os.environ, {'BOOT_ARM_GCC': str(compiler)}):
                self.assertEqual(arm_gcc(), compiler)
                for runner in RUNNERS:
                    with self.subTest(runner=runner):
                        values = runpy.run_path(str(HERE / runner))
                        self.assertEqual(values['GCC'], compiler)
                        self.assertEqual(values.get('BIN', values['GCC'].parent), compiler.parent)
                        # Physical-root derives siblings directly; other gates use BIN.
                        tree = ast.parse((HERE / runner).read_text())
                        siblings = [n for n in ast.walk(tree) if isinstance(n, ast.BinOp)
                                    and isinstance(n.op, ast.Div)
                                    and ast.unparse(n.left) in ('BIN', 'GCC.parent')]
                        self.assertTrue(siblings, 'sibling tools remain compiler-relative')

    def test_invalid_override_never_falls_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            nonexec = directory / 'nonexec'
            nonexec.write_text('not executable\n')
            nonexec.chmod(0o644)
            invalid = ('', 'relative/bin/gcc', '~/bin/gcc', str(directory / 'missing'),
                       str(directory), str(nonexec))
            for value in invalid:
                with self.subTest(value=value), patch.dict(os.environ, {'BOOT_ARM_GCC': value}):
                    with self.assertRaisesRegex(ValueError, 'BOOT_ARM_GCC'):
                        arm_gcc()
                    for runner in RUNNERS:
                        with self.subTest(runner=runner):
                            with self.assertRaisesRegex(ValueError, 'BOOT_ARM_GCC'):
                                runpy.run_path(str(HERE / runner))

    def test_publication_anchor_detects_nonintegration_edits(self):
        publication = runpy.run_path(str(HERE / 'run_publication.py'))
        for runner, anchor in (('run_sequence.py', 'SEQUENCE_AST'),
                               ('run_physical_root.py', 'UNCHANGED_AST')):
            with self.subTest(runner=runner):
                text = (HERE / runner).read_text()
                normalize = publication['normalized_runner']
                expected = publication[anchor]
                self.assertEqual(normalize(ast.parse(text)), expected)
                changed = text.replace('GCC=arm_gcc()', "GCC=Path('/other/gcc')").replace(
                    'GCC = arm_gcc()', "GCC = Path('/other/gcc')")
                self.assertNotEqual(normalize(ast.parse(changed)), expected)
                self.assertNotEqual(normalize(ast.parse(text + '\nCASES = ()\n')), expected)


if __name__ == '__main__':
    unittest.main()
