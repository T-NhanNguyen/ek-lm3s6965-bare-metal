# Boot-only decision and opt-in dispatch images (experimental, no deployment)

Configure outside the source tree with an explicitly authorized raw 65-byte
SEC1 uncompressed P-256 **public** root:

```sh
cmake -S . -B /tmp/lm3s6965-boot-reset \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_BOOT_RESET=ON \
  -DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON \
  -DLM3S6965_BOOT_PUBLIC_KEY_FILE=/absolute/path/to/public.sec1
cmake --build /tmp/lm3s6965-boot-reset --target lm3s6965_boot_reset
```

`LM3S6965_BUILD_BOOT_RESET` defaults OFF. ON requires explicit-root production
selection; it never silently uses demo trust. A known public test fixture is
suitable only for authorized RnD, not deployment-grade provisioning.

## Opt-in checked dispatch image (not hardware accepted)

Use a separate build directory and the same explicitly selected PUBLIC root.
`LM3S6965_BUILD_BOOT_DISPATCH_IMAGE` defaults OFF and is mutually exclusive
with `LM3S6965_BUILD_BOOT_RESET`; the default and decision image still never
execute an application.

```sh
cmake -S . -B /tmp/lm3s6965-boot-dispatch \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_BOOT_RESET=OFF \
  -DLM3S6965_BUILD_BOOT_DISPATCH_IMAGE=ON \
  -DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON \
  -DLM3S6965_BOOT_PUBLIC_KEY_FILE=/absolute/path/to/public.sec1
cmake --build /tmp/lm3s6965-boot-dispatch --target lm3s6965_boot_dispatch
```

The distinct ELF/map/bin/hex basename is `lm3s6965_boot_dispatch`. It shares
boot linker, verifier/crypto, full-width enum ABI and image checks; the checker
uses `--dispatch` to require target handoff, prepare and terminal/startup closure
and reject application reset/main/constructor leakage. The unchanged 32 KiB
limit still applies. The pre-IPO dispatch attempt overflowed by **2,316 bytes**
with compile-time `-Os` already effective. Dispatch-only Release now requires
supported IPO/LTO across the executable and reachable source archives, links
explicitly at `-Os`, and roots real definitions required by the unchanged static
checks. Decision-image optimization and crypto configuration are unchanged.
**Post-IPO fit and linked/static acceptance remain pending**; no size savings
are claimed.
The signed selected application must use the existing continuity entry stub
and slot linker ABI, not an arbitrary structurally valid reset vector.

The dispatch entry initializes fresh boot-local target/handoff, observes the
actual masked privileged core, calls `boot_sequence_launch` exactly once and
passes its genuine runtime result and launch output to `boot_startup_prepare`,
then nonreturning `boot_startup_dispatch`. No second producer, selection,
watchdog configuration/feed or ordinary application RAM initialization occurs
in boot. Genuine reset, exclusive boot-only hardware ownership and stable
clock/storage remain mandatory; this is not debugger adoption or chainload.

Checkpoints 1..7 retain their old meanings. Dispatch adds 8 handoff-init failure,
9 observation failure, 10 launch failure, 11 checked launch ready, 12 prepare
failure, 13 prepared for dispatch. Sequence result is valid at 10..13;
`boot_entry_launch` only at 11..13. `boot_entry_decision` remains valid **only**
at decision state 7. State 13 is not proof of application entry or C_READY;
final dispatch validation may still fail (retained startup FAILED, no-feed stop).
Early failures also stop without feed, but do not prove WDT was configured.
No dispatch build, test, programming, hardware execution, timing or physical
acceptance is claimed by this integration; all physical results below concern
only the historical decision-and-stop image.

## Decision image evidence

The target uses only `linker/bootloader.ld`, a target-local
`--entry=Boot_Reset_Handler`, real verifier/Mbed TLS and bounded boot-local
newlib allocation support. It does not inherit BSP/application startup or
application linker flags. No constructors or `main` run. Reset masks interrupts
as its first instruction, initializes ordinary data/BSS only, and preserves
retained continuity/diagnostics until the shared sequence intentionally begins.
All exceptions and IRQ0..42 stop without feed. Reserved core vectors are zero;
reserved IRQ positions remain terminal as in `src/startup.c`.

Clock setup assumes genuine reset, exclusive privileged masked ownership and
the EK-LM3S6965's **8 MHz crystal**. It uses the existing 200/4 = nominal **50 MHz**
recipe and bounded delays/PLL polling. Local datasheet (July 15, 2014),
pp.184–185, 200–202 documents RCC2 authority and BYPASS2: bypass the active PLL
before disabling the divider, prepare RCC's raw enabled MAIN source, then clear
USERCC2. RCC profile/RCC2/lock readback and USECRL=49 readback must succeed before
flash initialization. Failure is terminal; there is no claimed fallback Hz.
Register checks establish configuration, not measured frequency accuracy.

Boot-local storage is disjoint/stable through stop. The sole shared sequence
owns retained producer/WDT, fresh reset capture, accounting and real verification.
It is never retried and there is no application dispatch or reset actuator.
Debugger-visible `boot_entry_state` publishes last: 1 starting, 2 clock failure,
3 flash failure, 4 manifest-read failure, 5 bind failure, 6 sequence failure,
7 decision. `boot_entry_flash_result` is valid at 3/4;
`boot_entry_sequence_result` at 6/7 (nested validity rules still apply), and
`boot_entry_decision` only at 7. The original WDT may expire after terminal stop.

The map is `<build>/src/lm3s6965_boot_reset.map`. A successful link runs
`scripts/check-boot-reset-image.py` to enforce exactly 59 mapped vectors, Thumb
ELF/reset entry, first `cpsid i`, load/RAM bounds and required real closure.
The unchanged linker rejects full FLASH closure above 32 KiB, including data LMA.
**Current Release image passes bounded size/static acceptance:** payload extent
`[0,0x797c)` is **31,100 bytes**, leaving **1,668 bytes** in the unchanged 32 KiB
partition. Initialized data LMA `0x792c` plus `0x50` bytes gives that exclusive
end. Shared `config/mbedtls_boot.h` selects `MBEDTLS_ECP_FIXED_POINT_OPTIM 0` and
`MBEDTLS_SHA256_SMALLER`: the same P-256/SHA-256 algorithms with smaller code at
the cost of speed. The historical 34,004-byte attempt (end `0x84d4`, **1,236-byte
overflow**) remains a failed configuration, superseded by this **2,904-byte
reduction**, not relabeled as passing. No partition enlargement or verifier
substitution was used.

The final ELF/post-link and independent load/vector/closure checks pass. Accepted
artifacts are `/tmp/lm3s6965-boot-reset-build/src/lm3s6965_boot_reset` (ELF),
`/tmp/lm3s6965-boot-reset-build/src/lm3s6965_boot_reset.bin` and
`/tmp/lm3s6965-boot-reset-build/src/lm3s6965_boot_reset.hex`.
The explicitly selected authorized RnD PUBLIC root is present byte-for-byte in
the ARM ELF; this is static evidence, not deployment-grade trust or execution
of that root on ARM.

### Installed matching-root pair and physical RUN decision (2026-10-20)

A separate rebuilt ELF at
`/tmp/lm3s6965-boot-reset-breathing-root/src/lm3s6965_boot_reset` has SHA256
`0b99507803656980e77d6c0c1678d91e79f636f49af9caa88031d436e67b1292`.
Its payload extent is again **31100 bytes**, `[0,0x797c)`, with 1668 bytes spare;
the raw65 selected root SHA256 is
`960d06c1aaa1206cf37ce8e8b0cd002dfa441b9324e25c8c5a0735651f12c389`.
Exact public bytes were extracted from the final ELF and compared. This matching
boot is now **installed**, together with both FTP foreground payloads A/release44
and B/release45 and their generated metadata, replacing the older RFC-root boot.
See [installed fixture outcome](boot-fixture-metadata.md) for exact identities
and ranges. Programming erased exactly **101 pages**, performed **seven writes**
and verified **98652 bytes**; metadata was invalidated first, staged/read back,
and published by the final four-byte commit at `0x80a4`.

One configured **SYSRESETREQ reset halt and one continue** reached terminal
PC `0x130`, PRIMASK1: state **7**, successful sequence and validity-gated **RUN
decision for A/release44**, base `0x8800`, size33532, payload SHA256
`f3c924428c3f3bf80a6cf5848034641c2624cf4d9d6586dea6b204a293015ce3`.
Actual slot-A signature verification succeeded. Raw RCC `0x01ce1380`, RCC2
`0x07802800` (USERCC2 clear) and USECRL `0x31` are consistent with the nominal
50 MHz configuration, **not frequency calibration**.

This satisfies the requested decision-and-stop increment, not application
execution. Cleanup last confirmed halted, removed the terminal breakpoint and
left the owned server alive; this is last-known state, not persistence evidence.
No manual feed/freeze or dispatch occurred. B fallback, physical LED/network
execution, retained startup/application initialization and watchdog timing remain
pending. At that checkpoint, separately bounded application dispatch was not
implemented or authorized. The opt-in integration above does not extend that
physical acceptance. The standalone release42
[breathing fixture](../examples/ab-boot-demo/slot-a-breathing/README.md) and its offline
results remain historical, not the installed A44 payload.

### Earlier compact-crypto gate (retained)

Five actual HOST Release/sanitizers-OFF CTests pass: `boot_crypto`, `boot_verify`,
`boot_reset_handler`, `boot_reset` and `boot_clock_setup`. Together with one
final-image static representative, the bounded gate is **6 pass / 0 fail /
0 invalid**, not six behavioral tests. HOST verification uses demo-root signed
fixtures, not execution of the configured ARM RnD key. This documentation update
reruns neither builds nor tests; further execution belongs to test-merit-gate.

**16 prebuilt libc_nano/libgcc enum warnings remain**, unsuppressed. Timing,
watchdog performance, heap high-water and stack peaks remain unmeasured; static
RAM/heap/stack bounds are not runtime resource evidence. No application dispatch,
application execution, new flash/write authority, Debug/full-suite acceptance or
production readiness is implied beyond the bounded physical decision above.
