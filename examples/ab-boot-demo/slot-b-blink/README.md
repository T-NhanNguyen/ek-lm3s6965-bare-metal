# Offline slot-B dual-blink STATUS LED fixture

> Counterpart of `../slot-a-breathing` for the checked-application-handoff
> trace. Retired source: `examples/boot-breathing-led` (slot-A breathing
> fixture). Wiring, entry, startup/watchdog boundary, link flags, filtered
> image generator logic and package/client shape match slot A; only the slot
> (`linker/app_b.ld`, `BASES["B"] = 0x24000`), the LED timing
> (`blink_pattern.h` dual-blink replacing the PWM envelope) and this README
> differ.

**Default OFF; no offline acceptance recorded. Not hardware ready.** The
installed boot image is decision-only: it does not dispatch this application.
Building, packaging or placing a payload in slot B cannot make that boot run
the LED. This fixture adds no launch/handoff changes, metadata, provisioning
or boot writes.

The commands below build the LED-only target; no public key or verifier is
required. The separate `lm3s6965_ab_ftp_b` production receiver variant is built
only with `LM3S6965_BUILD_AB_BOOT_DEMO_FTP=ON` in addition to the demo option.
Its production closure requirements are in the [demo README](../README.md).
The offline outcome and no-service boundary below describe the LED-only target,
not that FTP variant.

## Pin and dual-blink timing

The repository's `include/lm3s6965/led.h` maps `USER_LED` to **PF0**;
`src/led.c` enables GPIOF, selects digital GPIO output and uses masked writes.
`src/gpio.c` implements the same masked-address convention. This application
uses `user_led_initialize` / `user_led_write`, not a guessed pin or new driver.
The STATUS LED name here refers to that repository user LED. Physical polarity
and visible behavior have not been verified. Active-low wiring inverts the
levels but keeps the same timing shape.

Each blink cycle has 8 phases of nominal 100 ms shape: ON, OFF, ON, OFF x5
(nominal 100/100/100/500 ms double blink), with 16 frames per phase, 64 slices
per frame and a bounded 32-iteration NOP hold per slice. All inner loops are
bounded; the outer application loop repeats indefinitely. `blink_pattern.h`
exposes the pure pattern for deterministic inspection. No timer, SysTick, ISR,
interrupt unmask, sleep or peripheral PWM is used. No frequency, blink period,
perceptual timing or actual 50 MHz clock is claimed: compiler/MMIO overhead
and physical timing are unmeasured.

## Startup and watchdog boundary

The target uses **only** `lm3s6965_application_entry` for its one vector table
and inherited `Application_Reset_Handler`, plus the existing boot-startup
archive. It does not link the BSP OBJECT library (which carries a second
startup and the default linker). The sole slot linker is `linker/app_b.ld`,
with the continuity entry also selected as the ELF entry and GNU maximum page
size `0x100` for the strict ELF packager.

Entry must come from a separately accepted authenticated dispatch with the
original watchdog and boot-FLASH/retained-only callbacks, expected core state
and stable truthful inherited clock. Cold reset or direct debugger entry is
not supported. Existing startup validates that continuity before normal data
copy, BSS clear and constructors. The linker preserves NOLOAD continuity1024
at `0x2000DBC0` and diagnostic64 at `0x2000DFC0`, below the 8 KiB stack.
After LED initialization, `boot_startup_c_ready` validates/publishes APP-only
readiness without feeding; rejection stops. `system_control.c` is linked only
for the LED peripheral gate helper; main never calls PLL configuration.

**No watchdog service is implemented.** The existing foreground inherited-owner
contract requires genuine Ethernet, timer-service and FTP completion in the
same live epoch before service (`examples/ethernet-ftp/foreground_owner.c`).
Blink iterations are none of those predicates. This fixture does not fabricate
evidence, feed blindly, restart/reconfigure/disable the watchdog, reset a budget,
or claim a timer owned by boot. If later dispatched with a live watchdog,
expiry/reset is expected unless a separately reviewed owner integration exists;
the retained startup timer may also expire. Sustained physical blinking is
therefore **not accepted or hardware ready**, even if this payload links.
Correct continuity closure may substantially exceed 1 KiB; do not replace it
with cold startup to chase size. See [startup contract](../../../docs/boot-startup.md).

## Offline outcome

No gate has been recorded for slot B: no payload size, SHA256, package,
signing, envelope or tamper-rejection outcome exists. The retained slot-A
outcome in `../slot-a-breathing/README.md` is not transferable to this slot.
The host pattern check below is not physical LED behavior.

## Reference commands (not new execution authority)

A key-free Release configure and LED-only build were checked after the move;
no acceptance gate or test execution is recorded. These are reproduction
references only. Test execution belongs to separately authorized test-merit-gate:

```sh
cmake -S . -B /tmp/lm3s6965-blink-b \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_AB_BOOT_DEMO=ON
cmake --build /tmp/lm3s6965-blink-b --target lm3s6965_boot_blink_b
cc -std=c11 -Wall -Wextra -Werror examples/ab-boot-demo/slot-b-blink/test_blink_pattern.c \
  -o /tmp/lm3s6965-blink-pattern
/tmp/lm3s6965-blink-pattern
```

Final ELF (no `.elf` suffix):
`/tmp/lm3s6965-blink-b/examples/ab-boot-demo/slot-b-blink/lm3s6965_boot_blink_b`.
The same directory receives `.map`, `.bin` and `.hex`. These outputs are not
signed packages. The fixture-local `generate-slot-b-images.py` validates the ELF
with the existing packager parser, then passes explicit `--only-section` arguments
to objcopy for nonzero allocated file-backed sections with FLASH LMA in
`[0x24000, 0x3F400)`, using `0xff` gap fill. Empty sections (especially the
zero-size SRAM `.data` that made unfiltered objcopy emit a roughly 512 MiB
binary) and NOBITS are excluded. Future nonempty `.data` is included by its
FLASH LMA, not its SRAM VMA. Nonempty loads outside the payload are rejected;
SRAM NOLOAD reservations remain valid. No shared helper or linker change is
needed. Canonical ELF/bin equivalence remains an acceptance gate. This producer
fix is fixture-local: no shared objcopy helper or linker policy was changed.

Inspect final vectors, one vector-table symbol, reset stub,
load LMAs, retained reservations and absence of reachable PLL calls using
`arm-none-eabi-readelf -lW -SW`, `arm-none-eabi-nm` and
`arm-none-eabi-objdump -d` on that **exact final ELF**.

The existing packager explicitly supports `--input-format elf`; it validates
final PT_LOAD/section/LMA/vector structure rather than relying on the suffix.
A key-free parser-only check for that exact artifact is available for the gate:

```sh
python3 - <<'PY'
from pathlib import Path
from tools.boot.package import canonical_payload, read_input
elf = Path('/tmp/lm3s6965-blink-b/examples/ab-boot-demo/slot-b-blink/lm3s6965_boot_blink_b')
payload = canonical_payload(read_input(elf), 'B', 'elf')
print('Validated slot-B canonical payload bytes:', len(payload))
PY
```

Any future signing would use that exact path as `--input`,
`--input-format elf --slot B --release <n>`, an explicitly selected public root
and retained external key, per [packaging](../../../docs/boot-package.md).
This documentation accesses no key and grants no new signing authority. Offline
signature acceptance is not dispatch, service-gap, clock, hardware or sustained-LED
acceptance. See also [decision-only boot](../../../docs/boot-reset-image.md).
