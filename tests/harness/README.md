# HOST integrity launchpad

The [canonical HOST procedure](../../docs/tools-testing.md#host-integrity-runner)
explains selection, prerequisites, artifacts, provenance and pass/fail/invalid.
From the repository root, with installed dependencies and tools:

```sh
python3 tests/harness/runner.py --all --budget-seconds 900 --failures-only
```

Use repeatable `--case target:name` for narrow changes. [manifest.json](manifest.json)
is the case authority. This performs offline builds and QEMU, not hardware,
live networking or downloads. A full roster overlaps Ethernet tests.

Configuration-only regression, no builds or QEMU:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/harness -p 'test_config.py' -v
```

A documented command is not a current result. Historical runner evidence does
not verify revised source or documentation fingerprints.
