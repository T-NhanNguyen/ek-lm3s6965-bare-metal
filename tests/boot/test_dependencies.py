#!/usr/bin/env python3
"""Offline isolated dependency fixtures; never touch repository dependencies."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('dependencies',
    Path(__file__).resolve().parents[2] / 'scripts/setup-dependencies.py')
dep = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dep)


class Dependencies(unittest.TestCase):
    def inherited_environment(self):
        return os.environ

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        # -C does not override inherited Git repository/index redirection or
        # injected config. Drop the whole Git namespace before the FIRST init;
        # preserve PATH and other system-tool environment (e.g. macOS SDK vars).
        self.env = {k: v for k, v in self.inherited_environment().items()
                    if not k.startswith('GIT_')}
        home = self.base / 'home'
        home.mkdir()
        self.env.update(HOME=str(home), XDG_CONFIG_HOME=str(home),
                        GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_CONFIG_SYSTEM=os.devnull,
                        GIT_ALLOW_PROTOCOL='file')
        # Patch only the imported helper's subprocess binding, not process-wide
        # os.environ or subprocess.run (other tests must retain their environment).
        run = subprocess.run

        def fixture_run(*args, **kwargs):
            kwargs['env'] = {**self.env, 'GIT_OPTIONAL_LOCKS': '0'}
            return run(*args, **kwargs)

        helper_patch = patch.object(dep, 'subprocess', SimpleNamespace(run=fixture_run, PIPE=subprocess.PIPE))
        helper_patch.start()
        self.helper_patch = helper_patch
        self.addCleanup(helper_patch.stop)
        self.root = self.base / 'parent'
        self.root.mkdir()
        (self.root / 'third_party').mkdir()
        self.git(self.root, 'init', '-q')
        self.pins = {}
        modules = []
        for i, name in enumerate(dep.PINS):
            upstream = self.base / f'upstream{i}'
            upstream.mkdir()
            self.git(upstream, 'init', '-q')
            (upstream / 'source.c').write_text('fixture\n')
            self.git(upstream, 'add', '.')
            self.git(upstream, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                     'commit', '-qm', 'fixture')
            pin = self.git(upstream, 'rev-parse', 'HEAD')
            self.pins[name] = (pin, str(upstream))
            self.git(self.root, 'update-index', '--add', '--cacheinfo', f'160000,{pin},{name}')
            modules.append(f'[submodule "{name}"]\n path = {name}\n url = {upstream}\n')
        (self.root / '.gitmodules').write_text(''.join(modules))
        self.git(self.root, 'add', '.gitmodules')
        self.git(self.root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'parent pins')
        pins_patch = patch.object(dep, 'PINS', self.pins)
        pins_patch.start()
        self.addCleanup(pins_patch.stop)

    def git(self, root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True,
                                       stderr=subprocess.PIPE, env=self.env).strip()

    def archive(self, extras=(), version='3.6.7'):
        archive = self.base / 'fixture.tar.bz2'
        files = {'LICENSE': b'fixture',
                 'include/mbedtls/build_info.h':
                 f'#define MBEDTLS_VERSION_STRING "{version}"\n#define MBEDTLS_VERSION_NUMBER 0x03060700\n'.encode()}
        files.update({f'library/{m}.c': b'fixture' for m in dep.MODULES})
        files.update({f'include/mbedtls/{h}.h': b'fixture' for h in dep.HEADERS if h != 'build_info'})
        with tarfile.open(archive, 'w:bz2') as tar:
            for name, data in files.items():
                member = tarfile.TarInfo(dep.ARCHIVE_ROOT + '/' + name)
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
            for member in extras:
                tar.addfile(member, io.BytesIO(b'x') if member.isfile() else None)
        return archive

    def install(self, archive):
        with patch.object(dep, 'SHA256', hashlib.sha256(archive.read_bytes()).hexdigest()):
            return dep.setup(self.root, archive=archive)

    def assert_clean_temp(self):
        self.assertFalse(list((self.root / 'third_party').glob('.dependency-setup-*')))

    def test_local_install_and_zero_network_check(self):
        self.assertEqual(self.install(self.archive()), 0)
        with patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            self.assertEqual(dep.setup(self.root, check=True), 0)
            self.assertEqual(dep.setup(self.root), 0)
        self.assert_clean_temp()

    def test_production_hostile_environment_preserves_external_repo(self):
        # Fixture creation is clean; the REAL helper must defend its own Git calls.
        self.helper_patch.stop()
        outside = self.base / 'sentinel'
        outside.mkdir()
        self.git(outside, 'init', '-q')
        (outside / 'sentinel.txt').write_bytes(b'external user work\n')
        self.git(outside, 'add', '.')
        self.git(outside, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'sentinel')
        index = self.base / 'external.index'
        index.write_bytes((outside / '.git/index').read_bytes())
        config = self.base / 'injected.config'
        config.write_text(f'[core]\n worktree = {outside}\n[protocol "file"]\n allow = never\n')

        def snapshot():
            return ({str(p.relative_to(outside)): p.read_bytes()
                     for p in outside.rglob('*') if p.is_file()}, index.read_bytes())

        before = snapshot()
        parent_index = (self.root / '.git/index').read_bytes()
        hostile = {**self.env, 'GIT_DIR': str(outside / '.git'),
                   'GIT_WORK_TREE': str(outside), 'GIT_INDEX_FILE': str(index),
                   'GIT_COMMON_DIR': str(outside / '.git'),
                   'GIT_OBJECT_DIRECTORY': str(outside / '.git/objects'),
                   'GIT_ALTERNATE_OBJECT_DIRECTORIES': str(outside / '.git/objects'),
                   'GIT_CONFIG': str(config), 'GIT_CONFIG_GLOBAL': str(config),
                   'GIT_CONFIG_SYSTEM': str(config), 'GIT_CONFIG_COUNT': '2',
                   'GIT_CONFIG_KEY_0': 'core.worktree', 'GIT_CONFIG_VALUE_0': str(outside),
                   'GIT_CONFIG_KEY_1': 'protocol.file.allow', 'GIT_CONFIG_VALUE_1': 'never',
                   'GIT_CONFIG_PARAMETERS': "'core.worktree=" + str(outside) + "'"}
        with patch.dict(os.environ, hostile, clear=True), \
             patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            self.assertEqual(self.install(self.archive()), 0)
            self.assertEqual(dep.setup(self.root, check=True), 0)
            self.assertEqual(dep.setup(self.root), 0)
            for name, (pin, _) in self.pins.items():
                self.assertEqual(Path(dep.git(self.root / name, 'rev-parse', '--show-toplevel')).resolve(),
                                 (self.root / name).resolve())
                self.assertEqual(dep.git(self.root / name, 'rev-parse', 'HEAD'), pin)
            mbed_before = {str(p.relative_to(self.root)): p.read_bytes()
                           for p in (self.root / 'third_party/mbedtls').rglob('*') if p.is_file()}
            path = self.root / 'third_party/lwip'
            (path / 'source.c').write_bytes(b'dirty user work')
            dirty_index = (self.root / '.git/modules/third_party/lwip/index').read_bytes()
            with self.assertRaisesRegex(dep.SetupError, 'dirty'):
                dep.setup(self.root, archive=self.archive())
            self.assertEqual((path / 'source.c').read_bytes(), b'dirty user work')
            self.assertEqual((self.root / '.git/modules/third_party/lwip/index').read_bytes(), dirty_index)
            self.git(path, 'add', 'source.c')
            self.git(path, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                     'commit', '-qm', 'wrong pin')
            wrong_head = self.git(path, 'rev-parse', 'HEAD')
            wrong_index = (self.root / '.git/modules/third_party/lwip/index').read_bytes()
            with self.assertRaisesRegex(dep.SetupError, 'expected'):
                dep.setup(self.root, archive=self.archive())
            self.assertEqual(self.git(path, 'rev-parse', 'HEAD'), wrong_head)
            self.assertEqual((self.root / '.git/modules/third_party/lwip/index').read_bytes(), wrong_index)
            self.assertEqual({str(p.relative_to(self.root)): p.read_bytes()
                              for p in (self.root / 'third_party/mbedtls').rglob('*') if p.is_file()},
                             mbed_before)
        self.assertEqual(snapshot(), before)
        self.assertEqual((self.root / '.git/index').read_bytes(), parent_index)
        self.assert_clean_temp()

    def test_config_errors_fail_preflight_with_stderr(self):
        original = dep.subprocess.run
        for code in (2, 128):
            with self.subTest(code=code):
                def failed_config(args, **kwargs):
                    if args[3:5] == ['config', '--get']:
                        return subprocess.CompletedProcess(args, code, '', 'fatal: malformed config fixture')
                    return original(args, **kwargs)
                with patch.object(dep.subprocess, 'run', side_effect=failed_config), \
                     patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
                    with self.assertRaisesRegex(dep.SetupError, 'fatal: malformed config fixture'):
                        dep.setup(self.root, archive=self.archive())
                self.assertFalse((self.root / 'third_party/mbedtls').exists())
                self.assert_clean_temp()

    def test_missing_check_zero_access(self):
        with patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            self.assertEqual(dep.setup(self.root, check=True), 1)
        self.assert_clean_temp()

    def test_bad_checksum_no_submodule_mutation(self):
        with self.assertRaisesRegex(dep.SetupError, 'SHA256'):
            dep.setup(self.root, archive=self.archive())
        for name in self.pins:
            self.assertFalse((self.root / name).exists())
        self.assert_clean_temp()

    def test_unsafe_archives(self):
        for name, kind in [(dep.ARCHIVE_ROOT + '/../escape', tarfile.REGTYPE),
                           ('/escape', tarfile.REGTYPE),
                           (dep.ARCHIVE_ROOT + '/link', tarfile.SYMTYPE),
                           (dep.ARCHIVE_ROOT + '/hard', tarfile.LNKTYPE),
                           (dep.ARCHIVE_ROOT + '/device', tarfile.CHRTYPE),
                           (dep.ARCHIVE_ROOT + '/LICENSE', tarfile.REGTYPE)]:
            with self.subTest(name=name):
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = 1 if kind == tarfile.REGTYPE else 0
                member.linkname = '../../escape'
                with self.assertRaises(dep.SetupError):
                    self.install(self.archive([member]))
                self.assert_clean_temp()
                self.assertFalse((self.base / 'escape').exists())
                self.assertFalse((self.root / 'third_party/mbedtls').exists())

    def test_wrong_version_and_missing_closure(self):
        with self.assertRaisesRegex(dep.SetupError, 'expected Mbed'):
            self.install(self.archive(version='3.6.8'))
        self.install(self.archive())
        (self.root / 'third_party/mbedtls/library/ecdsa.c').unlink()
        with self.assertRaisesRegex(dep.SetupError, 'required file'):
            dep.setup(self.root, check=True)
        self.assert_clean_temp()

    def test_refuse_existing_before_download(self):
        for path in ['third_party/lwip', 'third_party/mbedtls']:
            dest = self.root / path
            dest.mkdir()
            marker = dest / 'mine'
            marker.write_bytes(b'preserve me')
            with patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
                with self.assertRaises(dep.SetupError):
                    dep.setup(self.root)
            self.assertEqual(marker.read_bytes(), b'preserve me')
            marker.unlink()
            dest.rmdir()

    def test_dirty_and_wrong_pin_preserved(self):
        self.install(self.archive())
        path = self.root / 'third_party/lwip'
        source = path / 'source.c'
        source.write_text('dirty bytes')
        with self.assertRaisesRegex(dep.SetupError, 'dirty'):
            dep.setup(self.root)
        self.assertEqual(source.read_text(), 'dirty bytes')
        self.git(path, 'add', 'source.c')
        self.git(path, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'wrong pin')
        head = self.git(path, 'rev-parse', 'HEAD')
        with self.assertRaisesRegex(dep.SetupError, 'expected'):
            dep.setup(self.root)
        self.assertEqual(self.git(path, 'rev-parse', 'HEAD'), head)

    def test_all_destinations_preflight_before_mutation(self):
        names = list(self.pins)
        later = self.root / names[1]
        later.mkdir()
        (later / 'user-work').write_bytes(b'untouched')
        with patch.object(dep.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            with self.assertRaises(dep.SetupError):
                dep.setup(self.root, archive=self.archive())
        self.assertFalse((self.root / names[0]).exists())
        self.assertEqual((later / 'user-work').read_bytes(), b'untouched')
        self.assert_clean_temp()

    def test_symlink_destination_and_git_failure_cleanup(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (outside / 'marker').write_bytes(b'untouched')
        link = self.root / 'third_party/mbedtls'
        link.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(dep.SetupError, 'symlink'):
            dep.setup(self.root)
        self.assertEqual((outside / 'marker').read_bytes(), b'untouched')
        link.unlink()
        original = dep.git

        def failing_git(root, *args, **kwargs):
            if args[:2] == ('submodule', 'update'):
                raise dep.SetupError('simulated clone failure')
            return original(root, *args, **kwargs)

        with patch.object(dep, 'git', side_effect=failing_git):
            with self.assertRaisesRegex(dep.SetupError, 'simulated clone'):
                self.install(self.archive())
        self.assert_clean_temp()
        self.assertFalse(link.exists())
        self.assertEqual((outside / 'marker').read_bytes(), b'untouched')

    def test_parent_toplevel_and_head_pins(self):
        nested = self.root / 'nested'
        (nested / 'third_party').mkdir(parents=True)
        with self.assertRaisesRegex(dep.SetupError, 'intended parent checkout'):
            dep.setup(nested)
        name = next(iter(self.pins))
        self.git(self.root, 'update-index', '--cacheinfo', f'160000,{"1" * 40},{name}')
        self.git(self.root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'wrong HEAD pin')
        self.git(self.root, 'update-index', '--cacheinfo', f'160000,{self.pins[name][0]},{name}')
        index = (self.root / '.git/index').read_bytes()
        with self.assertRaisesRegex(dep.SetupError, 'parent HEAD gitlink'):
            dep.setup(self.root, archive=self.archive())
        self.assertEqual((self.root / '.git/index').read_bytes(), index)
        self.assertFalse((self.root / name).exists())
        self.assertFalse((self.root / 'third_party/mbedtls').exists())

    def test_parent_pin_mismatch_and_empty_mbed_preflight(self):
        name = next(iter(self.pins))
        pin = '1' * 40
        self.git(self.root, 'update-index', '--cacheinfo', f'160000,{pin},{name}')
        with self.assertRaisesRegex(dep.SetupError, 'parent index'):
            dep.setup(self.root)
        self.git(self.root, 'update-index', '--cacheinfo', f'160000,{self.pins[name][0]},{name}')
        (self.root / 'third_party/mbedtls').mkdir()
        with self.assertRaisesRegex(dep.SetupError, 'existing empty'):
            dep.setup(self.root)
        self.assertFalse((self.root / name).exists())


class ProductionGit(unittest.TestCase):
    def test_environment_and_config_exit_contract(self):
        preserved = {'HOME': '/trusted/home', 'XDG_CONFIG_HOME': '/trusted/config',
                     'GIT_SSH_COMMAND': 'ssh -F trusted', 'GIT_ASKPASS': 'trusted-helper',
                     'HTTPS_PROXY': 'http://trusted-proxy', 'GIT_ALLOW_PROTOCOL': 'https'}
        injected = {'GIT_DIR': '/outside', 'GIT_INDEX_FILE': '/outside/index',
                    'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.worktree',
                    'GIT_CONFIG_VALUE_0': '/outside'}
        with patch.dict(os.environ, {**preserved, **injected}, clear=True):
            for code, output in ((0, 'value\n'), (1, '')):
                with patch.object(dep.subprocess, 'run', return_value=
                                  subprocess.CompletedProcess([], code, output, '')) as run:
                    self.assertEqual(dep.git(Path('/intended'), 'config', '--get', 'key',
                                             missing_ok=True), 'value' if code == 0 else None)
                    self.assertEqual(run.call_args.kwargs['env'],
                                     {**preserved, 'GIT_OPTIONAL_LOCKS': '0'})
            with patch.object(dep.subprocess, 'run', side_effect=OSError('cannot launch git')):
                with self.assertRaisesRegex(OSError, 'cannot launch git'):
                    dep.git(Path('/intended'), 'config', '--get', 'key', missing_ok=True)


class FixtureEnvironment(unittest.TestCase):
    def test_inherited_git_environment_cannot_escape_fixture(self):
        ambient = dict(os.environ)
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            home = base / 'home'
            home.mkdir()
            # Bootstrap the sacrificial repo with an explicitly safe environment;
            # never point a regression at this project's repo or user's HOME.
            safe = {k: v for k, v in ambient.items() if not k.startswith('GIT_')}
            safe.update(HOME=str(home), XDG_CONFIG_HOME=str(home),
                        GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_CONFIG_SYSTEM=os.devnull)
            outside = base / 'sentinel'
            outside.mkdir()

            def git(*args):
                return subprocess.check_output(['git', '-C', str(outside), *args],
                                               env=safe, stderr=subprocess.PIPE)

            git('init', '-q')
            (outside / 'sentinel.txt').write_bytes(b'external user work\n')
            git('add', '.')
            git('-c', 'user.name=Sentinel', '-c', 'user.email=sentinel@example.invalid',
                'commit', '-qm', 'sentinel')
            index = base / 'external.index'
            index.write_bytes((outside / '.git/index').read_bytes())
            config = base / 'injected.config'
            config.write_text(f'[core]\n worktree = {outside}\n[protocol "file"]\n allow = never\n')
            (home / '.gitconfig').write_bytes(config.read_bytes())
            (home / 'git').mkdir()
            (home / 'git/config').write_bytes(config.read_bytes())

            def snapshot():
                files = {str(p.relative_to(outside)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in outside.rglob('*') if p.is_file()}
                return files, hashlib.sha256(index.read_bytes()).hexdigest()

            before = snapshot()
            redirects = {'GIT_DIR': str(outside / '.git'), 'GIT_WORK_TREE': str(outside),
                         'GIT_INDEX_FILE': str(index)}
            configs = {'HOME': str(home), 'XDG_CONFIG_HOME': str(home),
                       'GIT_CONFIG_GLOBAL': str(config), 'GIT_CONFIG_SYSTEM': str(config),
                       'GIT_CONFIG': str(config), 'GIT_CONFIG_COUNT': '2',
                       'GIT_CONFIG_KEY_0': 'core.worktree', 'GIT_CONFIG_VALUE_0': str(outside),
                       'GIT_CONFIG_KEY_1': 'protocol.file.allow', 'GIT_CONFIG_VALUE_1': 'never',
                       'GIT_CONFIG_PARAMETERS': "'core.worktree=" + str(outside) + "'"}
            ancillary = {'GIT_COMMON_DIR': str(outside / '.git'),
                         'GIT_OBJECT_DIRECTORY': str(outside / '.git/objects'),
                         'GIT_ALTERNATE_OBJECT_DIRECTORIES': str(outside / '.git/objects'),
                         'GIT_NAMESPACE': 'sentinel', 'GIT_WORK_TREE': str(outside),
                         'GIT_CEILING_DIRECTORIES': str(base), 'GIT_ALLOW_PROTOCOL': 'https'}
            cases = (redirects,
                     {k: configs[k] for k in ('HOME', 'XDG_CONFIG_HOME')},
                     {k: configs[k] for k in ('GIT_CONFIG_GLOBAL', 'GIT_CONFIG_SYSTEM')},
                     {k: v for k, v in configs.items() if k.startswith('GIT_CONFIG_')
                      and k != 'GIT_CONFIG_PARAMETERS'},
                     {'GIT_CONFIG_PARAMETERS': configs['GIT_CONFIG_PARAMETERS']},
                     {**redirects, **configs, **ancillary})
            for injected in cases:
                with self.subTest(variables=tuple(injected)):
                    fixture = Dependencies('test_local_install_and_zero_network_check')
                    fixture.inherited_environment = lambda: {**ambient, **injected}
                    try:
                        fixture.setUp()
                        fixture.test_local_install_and_zero_network_check()
                        fixture.test_dirty_and_wrong_pin_preserved()
                        self.assertEqual(fixture.env.get('PATH'), ambient.get('PATH'))
                        self.assertEqual({k for k in fixture.env if k.startswith('GIT_')},
                                         {'GIT_CONFIG_NOSYSTEM', 'GIT_CONFIG_GLOBAL',
                                          'GIT_CONFIG_SYSTEM', 'GIT_ALLOW_PROTOCOL'})
                        self.assertEqual(Path(fixture.git(fixture.root, 'rev-parse',
                                                          '--show-toplevel')).resolve(),
                                         fixture.root.resolve())
                        self.assertFalse((outside / 'source.c').exists())
                    finally:
                        fixture.doCleanups()
                        self.assertEqual(snapshot(), before, 'external HEAD/index/files changed')
                        self.assertEqual(dict(os.environ), ambient, 'process environment leaked')


if __name__ == '__main__':
    with contextlib.redirect_stdout(io.StringIO()):
        unittest.main()
