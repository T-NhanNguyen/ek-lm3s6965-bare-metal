#!/usr/bin/env python3
"""Explicit dependency acquisition. Standard library only; --check never fetches."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

PINS = {
    'third_party/FreeRTOS-Kernel': ('3a22924e0a9ddbbc8b0758881c33b3422a5cc20d',
                                  'https://github.com/FreeRTOS/FreeRTOS-Kernel.git'),
    'third_party/lwip': ('77dcd25a72509eb83f72b033d219b1d40cd8eb95',
                         'https://git.savannah.nongnu.org/git/lwip.git'),
}
VERSION = '3.6.7'
ARCHIVE_ROOT = 'mbedtls-3.6.7'
SHA256 = 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'
URL = ('https://github.com/Mbed-TLS/mbedtls/releases/download/'
       'mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2')
MODULES = ('asn1parse asn1write bignum bignum_core constant_time '
           'ecdsa ecp ecp_curves platform_util sha256').split()
HEADERS = ('build_info bignum ecdsa ecp check_config sha256').split()


class SetupError(Exception):
    pass


def git_environment():
    # -C alone cannot constrain inherited repository/index/object redirection.
    # Preserve ordinary HOME/global config and credential/SSH/proxy transport.
    redirects = {'GIT_DIR', 'GIT_WORK_TREE', 'GIT_COMMON_DIR', 'GIT_INDEX_FILE',
                 'GIT_OBJECT_DIRECTORY', 'GIT_ALTERNATE_OBJECT_DIRECTORIES',
                 'GIT_NAMESPACE', 'GIT_CEILING_DIRECTORIES',
                 'GIT_DISCOVERY_ACROSS_FILESYSTEM', 'GIT_IMPLICIT_WORK_TREE',
                 'GIT_PREFIX', 'GIT_SHALLOW_FILE', 'GIT_REPLACE_REF_BASE'}
    return {**{k: v for k, v in os.environ.items()
               if k not in redirects and k != 'GIT_CONFIG' and not k.startswith('GIT_CONFIG_')},
            'GIT_OPTIONAL_LOCKS': '0'}


def git(root, *args, missing_ok=False):
    result = subprocess.run(['git', '-C', str(root), *args], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=git_environment())
    if missing_ok and result.returncode == 1:
        return None
    if result.returncode:
        raise SetupError(result.stderr.strip() or 'git command failed')
    return result.stdout.strip()


def missing(path):
    if path.is_symlink():
        raise SetupError(f'{path}: symlink refused; preserved')
    if not path.exists():
        return True
    if not path.is_dir():
        raise SetupError(f'{path}: not a directory; preserved')
    return not any(path.iterdir())


def check_mbed(path):
    required = ['LICENSE'] + [f'library/{m}.c' for m in MODULES]
    required += [f'include/mbedtls/{h}.h' for h in HEADERS]
    for name in required:
        p = path / name
        if not p.is_file() or p.is_symlink():
            raise SetupError(f'{path}: missing/unsafe required file {name}; preserved')
    text = (path / 'include/mbedtls/build_info.h').read_text()
    if not re.search(r'^\s*#\s*define\s+MBEDTLS_VERSION_STRING\s+"3\.6\.7"\s*$', text, re.M):
        raise SetupError(f'{path}: expected Mbed TLS {VERSION}; preserved')
    if not re.search(r'^\s*#\s*define\s+MBEDTLS_VERSION_NUMBER\s+0x03060700\s*$', text, re.M):
        raise SetupError(f'{path}: wrong numeric version; preserved')


def preflight(root):
    # Validate every destination before downloading or updating any submodule.
    third = root / 'third_party'
    if third.is_symlink() or not third.is_dir():
        raise SetupError('third_party must be an existing real directory')
    if Path(git(root, 'rev-parse', '--show-toplevel')).resolve() != root.resolve():
        raise SetupError(f'{root}: not the intended parent checkout; preserved')
    pending = []
    for name, (pin, url) in PINS.items():
        if git(root, 'ls-tree', 'HEAD', '--', name) != f'160000 commit {pin}\t{name}':
            raise SetupError(f'{name}: parent HEAD gitlink differs from approved pin {pin}')
        expected = f'160000 {pin} 0\t{name}'
        if git(root, 'ls-files', '--stage', '--', name) != expected:
            raise SetupError(f'{name}: parent index gitlink differs from approved pin {pin}')
        for key, value in [('path', name), ('url', url)]:
            if git(root, 'config', '-f', '.gitmodules', '--get', f'submodule.{name}.{key}') != value:
                raise SetupError(f'{name}: unexpected .gitmodules {key}')
        configured = git(root, 'config', '--get', f'submodule.{name}.url', missing_ok=True)
        if configured is not None and configured != url:
            raise SetupError(f'{name}: local submodule URL override differs; preserved')
        path = root / name
        if missing(path):
            stored = Path(git(root, 'rev-parse', '--absolute-git-dir')) / 'modules' / name
            if stored.exists():
                raise SetupError(f'{name}: retained submodule git directory; inspect manually; preserved')
            pending.append(name)
            print(f'{name}: MISSING (pin {pin})')
        else:
            if not (path / '.git').exists():
                raise SetupError(f'{name}: unrecognized existing directory; preserved')
            if Path(git(path, 'rev-parse', '--show-toplevel')).resolve() != path.resolve():
                raise SetupError(f'{name}: not its own checkout; preserved')
            head = git(path, 'rev-parse', 'HEAD')
            if head != pin:
                raise SetupError(f'{name}: HEAD {head}, expected {pin}; preserved')
            if git(path, 'status', '--porcelain', '--untracked-files=all', '--ignored'):
                raise SetupError(f'{name}: dirty/untracked/ignored files; preserved; inspect manually')
            print(f'{name}: exact HEAD {pin}, clean; not a source-content authenticity audit')
    mbed = root / 'third_party/mbedtls'
    need_mbed = missing(mbed)
    if need_mbed and mbed.exists():
        raise SetupError(f'{mbed}: existing empty directory; remove manually if intended; preserved')
    if need_mbed:
        print(f'third_party/mbedtls: MISSING (official archive SHA256 {SHA256})')
    else:
        check_mbed(mbed)
        print('third_party/mbedtls: version 3.6.7 and required modules present; '
              'existing source-content integrity NOT authenticated offline')
    return pending, need_mbed


def extract_verified(archive, dest):
    # Read/check the very same bytes subsequently parsed, avoiding archive replacement races.
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SetupError('Mbed TLS archive SHA256 mismatch; nothing extracted')
    import io
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:bz2') as tar:
        members = tar.getmembers()
        seen = set()
        for member in members:
            name = member.name
            parts = PurePosixPath(name).parts
            if (not parts or name.startswith('/') or '\\' in name or
                    '..' in parts or parts[0] != ARCHIVE_ROOT or
                    not (member.isfile() or member.isdir())):
                raise SetupError(f'unsafe/unsupported archive member: {name}')
            canonical = '/'.join(parts)
            if canonical in seen:
                raise SetupError(f'duplicate archive path: {name}')
            seen.add(canonical)
        # Manual extraction: no links, devices, ownership, or permissions from the archive.
        for member in members:
            relative = PurePosixPath(member.name).parts[1:]
            target = dest.joinpath(*relative)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
    check_mbed(dest)


def setup(root, check=False, archive=None):
    pending, need_mbed = preflight(root)
    if check:
        return 1 if pending or need_mbed else 0
    if not pending and not need_mbed:
        return 0
    # Owned staging only. Failures never remove/reset existing user checkouts.
    with tempfile.TemporaryDirectory(prefix='.dependency-setup-', dir=root / 'third_party') as temp:
        stage = Path(temp) / 'mbedtls'
        if need_mbed:
            stage.mkdir()
            if archive is None:
                archive = Path(temp) / 'release.tar.bz2'
                print(f'Downloading official release asset: {URL}')
                with urllib.request.urlopen(URL, timeout=60) as response, archive.open('xb') as out:
                    shutil.copyfileobj(response, out)
            extract_verified(Path(archive), stage)
        for name in pending:
            # Never update existing checkouts; --checkout applies only to missing destinations.
            if not missing(root / name):
                raise SetupError(f'{name}: appeared since preflight; preserved')
            git(root, 'submodule', 'update', '--init', '--checkout', '--', name)
            pin = PINS[name][0]
            if git(root / name, 'rev-parse', 'HEAD') != pin:
                raise SetupError(f'{name}: installed HEAD is not approved pin; retained for inspection')
        if need_mbed:
            target = root / 'third_party/mbedtls'
            if target.exists():
                # Even an empty pre-existing directory is not removed by this helper.
                raise SetupError(f'{target}: existing empty directory; remove manually if intended; preserved')
            stage.rename(target)
            print(f'Mbed TLS installed from checksum-verified official archive ({SHA256})')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='offline read-only status; exit 1 if missing')
    parser.add_argument('--mbedtls-archive', type=Path, help='local official tar.bz2, same mandatory checksum')
    args = parser.parse_args()
    if args.check and args.mbedtls_archive:
        parser.error('--check does not read archives; omit --mbedtls-archive')
    try:
        return setup(Path(__file__).resolve().parents[1], args.check, args.mbedtls_archive)
    except (SetupError, OSError, tarfile.TarError, ValueError) as error:
        print(f'Dependency setup refused/failed: {error}\nNo existing sources reset or removed. '
              'Inspect reported paths manually; see docs/dependencies.md.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
