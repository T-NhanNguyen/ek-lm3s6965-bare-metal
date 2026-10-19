# Feature and test entry points

Choose the topic you are changing. This is not a run-everything-in-order checklist.
Commands below are owner entry points, not evidence of execution or acceptance.

## Feature examples

**Bare-metal board features.** Start in [`examples/baremetal`](../examples/baremetal/)
for OLED, buttons, LED and UART. The [OLED guide](../docs/oled.md),
[peripheral reference](../docs/peripherals.md) and [console guide](../docs/console.md)
cover the individual interfaces.

**FreeRTOS.** Start in [`examples/freertos`](../examples/freertos/) and the
[FreeRTOS guide](../docs/freertos.md). This is a separate application, not a boot
gate or a prerequisite for the bare-metal examples.

**Ethernet and FTP.** The [Ethernet guide](../docs/ethernet.md) separates
[`ethernet-link`](../examples/ethernet-link/) PHY checks from
[`ethernet-raw`](../examples/ethernet-raw/) frame diagnostics. The opt-in
[`ethernet-ftp`](../examples/ethernet-ftp/) example uses lwIP; see the
[FTP guide](../docs/ftp.md) and [reusable library guide](../docs/ftp-library.md).
Offline tests do not establish physical IP/FTP acceptance. Live traffic and
board resets require separate authorization.

## Offline HOST tests

Use the [HOST integrity runner guide](harness/README.md) for reporting and exact
`--case target:name` selection. Its `--all` remains available for a deliberately
requested full roster. The [Ethernet suite guide](../scripts/ethernet/README.md)
covers offline FTP, RAM files, platform shims, RAW fixtures and link evaluation.
These runners overlap; do not schedule both full rosters by default.

## Current boot owner gates

Read the [startup boundary and evidence limits](../docs/boot-startup.md) first.
Owner-selected checks cover the changed contract; they do not certify a
production reset path. The foreground [CMake targets](ethernet/foreground/CMakeLists.txt)
and [focused runner](ethernet/foreground/runner.py) own the application SysTick
switch, retained history and serialized Ethernet service checks. The registered
clock source is `clock_switch_test.c`.

For WDT gate setup and producer changes, select the focused cases below from the
repository root. They omit repeated retained regressions, not assertions within
a selected case. Execution belongs to the separately authorized test-merit-gate.

```sh
python3 tests/boot/run_wdt_setup.py \
  --case gate-order --case gate-ignored --case gate-exact --case arm-closure
python3 tests/boot/run_producer.py \
  --case producer-success --case producer-overflow \
  --case producer-admitted-failure --case producer-arm-closure \
  --budget-seconds 600
```

For shared-sequence changes, the owner selects repeatable `--case` entries in
[`run_sequence.py`](boot/run_sequence.py), such as `capture-fullword-mismatch`
or `fresh-clear`. For physical adapters or public-root composition, select in
[`run_physical_root.py`](boot/run_physical_root.py), such as `physical-map`,
`root-rebuild` or `verifier-roots`. Both require `--budget-seconds` (1–600).
Read each runner's `CASES` before selecting; a narrow case is not a full owner
gate. All four runners retain their existing `--all` rosters and semantics.

## Focused historical diagnostics

Retained WDT/runtime and startup/clock regressions remain explicitly selectable
in the setup and producer runners. Use them when their contracts change or the
owner requests the full roster, not as repeated routine prerequisites. The
[startup evidence history](../docs/boot-startup.md#historical-startup-source--test--evidence-ste)
and [resource limits](../docs/boot-resources.md) describe fixture scope.
Startup, launch, signing and resource fixtures stay in place. Historical passes
and minimized links do not prove current physical timing or complete boot fit.

## Source fingerprints

See [script documentation](../scripts/README.md) for the selected public inputs
and optional explicit tool hashing. This command is source-only by default:

```sh
python3 scripts/boot_fingerprints.py --output /tmp/boot-fingerprints.json
```

Publication regression entry point (Python standard library, no ARM tools):

```sh
python3 tests/boot/run_publication.py --all --budget-seconds 600 --failures-only
```

Use repeatable `--case` selections for narrow changes. Its six publication
representatives are separate from the physical-root and foreground clock gates.

It identifies source bytes, not test success or production readiness. Tools are
included only when explicitly requested; generating this manifest does not run
a boot gate.
