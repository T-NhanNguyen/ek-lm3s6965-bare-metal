# FTP stack written-watermark baseline

Run the offline fixtures first:

```sh
./scripts/ethernet/test.sh platform
```

## Opt-in build (no device access)

Use the populated pinned lwIP submodule and the official Arm GNU/newlib toolchain.
Both FTP and its measurement option default to OFF. From the repository root:

```sh
cmake -S . -B build/ftp-stack \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON \
  -DLM3S6965_FTP_STACK_MEASUREMENT=ON
cmake --build build/ftp-stack --target lm3s6965_ethernet_ftp --parallel 4
```

ELF: `build/ftp-stack/examples/ethernet-ftp/lm3s6965_ethernet_ftp`.
The 8 KiB linker reservation is unchanged: `[_stack_limit, _estack)` =
`[0x2000e000, 0x20010000)`. The reset **vector** points to an FTP-only naked
basic-assembly shim, painting ascending words with `0xa5c39e71`, preserving
PRIMASK in a register, masking interrupts, then restoring PRIMASK and branching
to the unchanged C `Reset_Handler`. There are no pushes/calls, stack loads,
or global RAM accesses before painting. This uses the C compiler; it does not
require CMake's ASM language. The ELF entry symbol remains `Reset_Handler`:
starting at that symbol directly bypasses painting; only a real reset-vector
boot establishes the baseline. Never call the painter on a running target.

Inspect the **final linked** vector, painter, literal pool and normal startup
with the matching toolchain's `arm-none-eabi-objdump` before accepting a new
compiler/build. Ordinary C startup is unchanged when measurement is OFF, as
is the independently compiled generic BSP even when FTP measurement is ON.
PRIMASK cannot mask NMI/HardFault. This is a reset-only baseline with valid SRAM;
those default handlers do not return. A fault/NMI during startup is not a valid
measurement run. No runtime repaint, target scan, or target printing is added.

Release retains `-Os`. Only the instrumented platform, lwIP, FTP TCP/core and
RAM-file targets receive `-fstack-usage`. Find their `.su` files under
`build/ftp-stack/**/CMakeFiles/`. These are compiler per-function frames, **not
summable whole-program peaks**: prebuilt newlib/libgcc, hardware exception frames
(32 bytes plus possible alignment), nesting, indirect calls and unwritten frame
slots are not established by them. The naked painter's reported zero is only
its software frame size.

## Authorized no-reset capture — not part of offline validation

**Do not execute this section during offline validation.** The user's existing
approval covers the planned single flash, reset-vector boot, 12-byte transfer
and no-reset readout; a review-only agent boundary does not revoke that approval.
This recipe neither flashes nor establishes which image is installed. After that
approved boot and workload, record the image hash/workload/idle duration. No
additional firmware, workload or hardware known-write control is implied. Stop any other debug server/console helper first. Do not use
`scripts/probe.sh`, link/console reset helpers, `program`, `reset`, target calls,
or target-side algorithms. Reset destroys the historical watermark.

1. Audit the installed OpenOCD `target/stellaris.cfg` and interface script before
   attaching. The example below uses the repository board config, disables all
   server ports, overrides its working area to zero **before init**, and replaces
   attach/detach, examine and reset hooks with explicit nonempty no-op bodies.
   In particular, retain the `reset-start` override: the installed Stellaris hook
   can read/write peripheral registers and change reset policy. Empty `{}` attach
   bodies are insufficient: `init_target_events` installs a default `halt 1000`
   when the body is empty. GDB remains disabled. Do not add connect-under-reset options.
   If a different OpenOCD/config needs reset or RAM writes to attach, stop.
2. Within the approval above, capture from the repository root into a **new**
   directory (not an existing dump). This halts without resetting and leaves the
   target halted; it changes debug state and compromises tick/timing evidence:

   ```sh
   (
   set -eu
   capture=$(mktemp -d "$PWD/build/ftp-stack/capture.XXXXXX")
   openocd -f openocd/board/ek-lm3s6965.cfg \
     -c 'gdb_port disabled; tcl_port disabled; telnet_port disabled' \
     -c 'reset_config none' \
     -c 'lm3s6965.cpu configure -work-area-size 0; foreach event {gdb-attach gdb-detach examine-start examine-end reset-start reset-end reset-init} {lm3s6965.cpu configure -event $event {# no-op}}' \
     -c "set capture {$capture}" \
     -c 'init; halt; wait_halt 5000; reg; foreach name {msp psp control primask xPSR pc sp lr basepri faultmask} {reg $name force}; dump_image $capture/stack.bin 0x2000e000 0x2000; shutdown' \
     > "$capture/openocd.log" 2>&1
   # Require OpenOCD exit 0 before scanning; retain the complete log.
   python3 scripts/ethernet/platform/stack_watermark.py "$capture/stack.bin"
   )
   ```

   Bare `reg` discovers/logs available register names and cache status; it is
   **not acquisition evidence**. The following loop explicitly uses `reg NAME
   force` to bypass the cache for MSP, PSP, CONTROL, PRIMASK, xPSR, PC and the
   additional SP/LR/BASEPRI/FAULTMASK context. Names are case-sensitive: use
   `xPSR`, not `xpsr`. Any unavailable name or failed forced read aborts the Tcl
   command before `dump_image`; nonzero OpenOCD exit aborts the shell before
   scanning. Do not substitute guessed numeric indices, ignore errors, or accept
   cache-only values. Retain the complete log, including all forced-read values.

   Installed OpenOCD 0.12.0 documentation (`share/info/openocd.info-2`, `reg`)
   verifies forced-read semantics. Its executable's ARMv7-M register strings
   corroborate `msp`, `psp`, `control`, `primask`, `xPSR`, `basepri`, `faultmask`;
   the installed docs use `pc`/`sp`, and the executable also contains `lr`.
   These are offline checks, not proof that this target exposes every name:
   discovery and successful forced reads at capture time are mandatory. The
   executable's `set_default_target_event` checks for an empty string; nonempty
   comment handlers survive `init_target_events` (verified without `init`).
   The command/config was audited offline, **not tested against a probe**.
   Audit changed versions again. Record whether xPSR's IPSR field (bits 8:0)
   indicates an ISR/exception; CONTROL and both stack pointers provide context.
   An attach/reset failure invalidates the capture; do not retry via reset.
3. Retain raw dump, forced registers, ELF hash, exact bounds, workload and scanner
   output as a **qualified written-watermark baseline**, not a validated end-to-end
   hardware capture path. Synthetic scanner fixtures and linked disassembly
   verification are offline evidence only. An on-device known-write positive
   control has **not been performed**; this limitation must accompany the result,
   but it is not a prerequisite to recording the approved qualified baseline and
   does not authorize an extra firmware image or workload.

## Meaning and limits

The scanner requires **exactly 8192 bytes and the exact bounds**, comparing
explicit little-endian bytes `71 9e c3 a5` from low to high addresses. The first
mismatch determines `0x20010000 - first_mismatch`, the **written watermark**.
It handles unaligned writes, isolated mismatches and sentinel holes above them;
it does not count changed bytes or stop at a hole scanning downward.

This is **not exact peak stack use or historical minimum MSP**. Startup, idle
and any SysTick activity since reset are included. Allocated-but-unwritten slots,
sentinel collisions and unexercised paths can conceal deeper use. A pristine
dump says no mismatches, not that the stack never moved. An untouched prefix is
not a proven safe margin. A mismatch at the bottom reports the full reservation,
not proof that overflow occurred (nor assurance that it did not). No measured
runtime margin, hardware capture, traffic result or worst-case safety is claimed
by this offline baseline.
