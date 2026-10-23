#!/usr/bin/env python3
"""Offline integration with two supplied, already signed packages; never signs.

Run only under test-merit-gate. The CLI must have been built with the explicitly
selected PUBLIC root. Codec/policy tests are separate from these crypto checks.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--package-a', type=Path, required=True)
    parser.add_argument('--release-a', type=int, required=True)
    parser.add_argument('--package-b', type=Path, required=True)
    parser.add_argument('--release-b', type=int, required=True)
    args = parser.parse_args()
    tool = str(args.tool.resolve())
    original = [args.package_a.read_bytes(), args.package_b.read_bytes()]
    for release in (args.release_a, args.release_b):
        assert 0 <= release <= 0xffffffff
    with tempfile.TemporaryDirectory(prefix='boot-metadata-integration-') as tmp:
        root = Path(tmp)
        packages = [root / 'a.pkg', root / 'b.pkg']
        releases = [args.release_a, args.release_b]
        for path, data in zip(packages, original):
            path.write_bytes(data)

        def invoke(success, output=None, selected=None, expected=None):
            selected = selected or packages
            expected = expected or releases
            command = [tool, '--package-a', str(selected[0]), '--release-a', str(expected[0]),
                       '--package-b', str(selected[1]), '--release-b', str(expected[1])]
            if output is None:
                command += ['--verify-only']
            else:
                command += ['--output-dir', str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            assert result.returncode == (0 if success else 1), (command, result.stdout, result.stderr)

        # Full real verifier acceptance is explicit and separate from generation.
        invoke(True)
        output = root / 'output'
        output.mkdir()
        invoke(True, output)
        page0 = (output / 'metadata-page0.bin').read_bytes()
        page1 = (output / 'metadata-page1.bin').read_bytes()
        assert len(page0) == len(page1) == 1024
        assert page0[168:] == b'\xff' * (1024 - 168)
        assert page1 == b'\xff' * 1024
        invoke(False, output)  # exclusive creation; must preserve both pages
        assert (output / 'metadata-page0.bin').read_bytes() == page0
        assert (output / 'metadata-page1.bin').read_bytes() == page1
        for slot in range(2):
            stale = releases.copy()
            stale[slot] ^= 1
            invoke(False, expected=stale)
            for offset in (96, 160):  # signature, then payload
                changed = bytearray(original[slot])
                changed[offset] ^= 1
                packages[slot].write_bytes(changed)
                failed_output = root / f'reject-{slot}-{offset}'
                failed_output.mkdir()
                invoke(False, failed_output)
                assert not list(failed_output.iterdir())
                packages[slot].write_bytes(original[slot])
            for malformed in (original[slot][:-1], original[slot] + b'\x00'):
                packages[slot].write_bytes(malformed)
                invoke(False)
            packages[slot].write_bytes(original[slot])
        invoke(False, selected=list(reversed(packages)))  # wrong physical slot
        # Explicit existing second-file collision must not remove that artifact.
        collision = root / 'collision'
        collision.mkdir()
        retained = collision / 'metadata-page1.bin'
        retained.write_bytes(b'retained')
        invoke(False, collision)
        assert retained.read_bytes() == b'retained'
        assert not (collision / 'metadata-page0.bin').exists()
    print('selected-root package verification, tamper/stale rejection and output preservation passed')


if __name__ == '__main__':
    main()
