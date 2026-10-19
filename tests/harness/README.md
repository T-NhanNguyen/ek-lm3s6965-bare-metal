# Offline HOST integrity runner

Run once from the repository (no device, flash, live peers or downloads):

```sh
python3 tests/harness/runner.py --all --budget-seconds 900 --failures-only
```

Select exact existing cases using repeatable `--case target:name` instead of
`--all`; see `manifest.json` for the explicit 37 CTest + 6 native + 13 Python
universe (56 cases) and per-case intent/class/assertion ledger. `--all` runs55 representatives:
`lwip-boot` includes every RAM assertion from `lwip` before its boot cases;
standalone `lwip` is compiled but its redundant execution is merged. It remains
explicitly selectable. Selection is validated before
builds/artifacts. `--human` sends the full readable log to stderr. Ordinary stdout
contains only JSONL; `--failures-only` suppresses passing records on stdout, not
in the complete report. This is the shared reporting entry point; direct CTest,
Python and legacy Ethernet shell invocations remain raw developer interfaces.

One fresh Release HOST build uses ASan/UBSan and test-only `-UNDEBUG`. Libraries
retain Release `NDEBUG`. Native Ethernet peers remain O1, not O3; their linked
boot libraries are Release. Preparation batches the six native binaries through
the existing build-only shell path, which also performs an incremental fixture
build (two build commands, one configuration). Python methods execute separately;
workflow's 23 methods stay nested inside its one CTest, never rerun standalone.
`boot_pc_ftp` runs the offline FTP transport/CLI suite with mocked sockets, not a
live FTP server; its internal methods are likewise nested, not extra rollup cases.
`boot_startup` covers HOST continuity policy. `boot_startup_target` contains
20 fresh QEMU processes (A/B × Os/Og × four positives plus one clock rejection),
not 20 CTests. It asserts zero startup watchdog writes and feeds against
independent pre-dispatch observations on the original context. Each incomplete
service preserves feeds, writes, epoch and partial evidence; complete evidence
permits exactly one feed and advances the live epoch without changing entry history.
No embedded scenario or assertion body is removed. Debug is intentionally NOT
reverified, and is not declared equivalent to Release.

The runner checks the exact CTest roster, reachable commands, compiler flags and
fixtures before behavioral execution. Each top-level case has a separate process
group. Product failures do not suppress later cases; missing tools, skips and
infrastructure failures stop execution and mark remaining cases invalid. The
single overall deadline includes setup/build; timeout kills/reaps the process
group and preserves completed records. There is no retry. Test-owned temporary
fixtures provide resets; every invocation has fresh build/native directories.

Artifacts are under ignored `.dev-vault/tests/runs/<timestamp>-host-integrity/`:
`run.log`, complete `report.jsonl`, exact `plan.json`, before/after provenance,
CTest roster/JUnit, and `execution.json` (budget and validity). Only marked runs
owned by this runner are pruned, retaining five. Source drift invalidates current
acceptance; `INVALIDATED` explicitly withdraws current-tree credit even when
previously emitted execution records describe completed processes.

Provenance content reads explicitly exclude every `resume.tex`, private vault,
`.pi`/Git metadata, build/build-* directories and Python caches even if tracked
or nonignored. Symlinks are hashed as link text, not followed into private files.
Git status still detects excluded-path drift; these exclusions do not authorize
publishing private/local files. Normal offline QEMU execution remains included;
there is no device access, live connection or download mode in this runner.

Offline runner-configuration and command-evidence regression (separate from the
56-case product manifest, no build, QEMU, sockets or devices):

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/harness -p 'test_config.py' -v
```

This checks the exact 37-name CTest/manifest roster, selection accounting, content
exclusions before reads, and symlink privacy without launching the runner. Mocked
startup commands check prelaunch command persistence, partial byte output on
timeout, return codes and bounded output overflow. It
requires its own gate result; syntax/static checks are not test passes.

A pass means all existing assertions in that top-level execution completed.
A binary crash cannot establish later internal scenarios. Internal Python
subtests and nested CTest methods are not independent JSONL/rollup cases. Mock
platform/raw TCP, RAM flash, synthetic heap and offline QEMU coverage must not be
reported as physical hardware, live transport, timing or target-fit acceptance.

Failure records retain the first recognizable assertion/sanitizer diagnostic.
If no source assertion is available, `at` points to the evidence log rather than
an invented production source location. Full JUnit/logs retain exact details.
Startup target commands persist command/timeout metadata before launch and save
separate stdout/stderr (up to 1 MiB per stream), combined `.log`, return code or
explicit timeout/launch-error status before raising. Overflow is infrastructure
invalid, not silently truncated acceptance. `STARTUP INFRASTRUCTURE:` diagnostics
are classified invalid by this runner; a target assertion remains a product
failure. A timeout never supplies a fabricated return code. These diagnostics
also remain visible to direct CTest gate owners in retained target artifacts.

The runner detects skipped unittest methods even when their enclosing CTest
returns success; skipped coverage is infrastructure-invalid, never passing.

Workflow's `Ran 23 tests` completeness check applies only to a zero-exit
`boot_pc_workflow` execution. A genuine nonzero assertion failure remains a
failure even if execution ends before that aggregate is printed. Essential
unavailable evidence (including missing/malformed JUnit, truncation, skips or
missing tools) remains invalid; neither failure nor invalid permits acceptance.
Both per-target summaries and the rollup include pass, fail and invalid counts.
Verbose CTest capture and the 10 MiB passed/failed output limits remain enabled.
These reporting protections require current execution evidence; historical
results do not verify a revised runner.
