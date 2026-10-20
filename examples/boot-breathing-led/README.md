# Offline slot-A breathing STATUS LED fixture

**Default OFF; bounded offline acceptance complete. Not hardware ready.** The installed
boot image is decision-only: it does not dispatch this application. Building,
packaging or placing a payload in slot A cannot make that boot run the LED.
This fixture adds no launch/handoff changes, metadata, provisioning or boot writes.

## Pin and software PWM

The repository's `include/lm3s6965/led.h` maps `USER_LED` to **PF0**;
`src/led.c` enables GPIOF, selects digital GPIO output and uses masked writes.
`src/gpio.c` implements the same masked-address convention. This application
uses `user_led_initialize` / `user_led_write`, not a guessed pin or new driver.
The STATUS LED name here refers to that repository user LED. Physical polarity
and visible behavior have not been verified. Active-low wiring reverses the
triangle but still gives gradual periodic brightness in the intended waveform.

Each PWM frame has 64 slices; electrical-high duty goes 0..64..1 over 128
phases, with 16 frames per phase. Each slice has a bounded 32-iteration NOP hold.
All inner loops are bounded; the outer application loop repeats indefinitely.
`pwm_envelope.h` exposes the pure envelope for deterministic inspection.
No timer, SysTick, ISR, interrupt unmask, sleep or peripheral PWM is used.
No frequency, breath period, perceptual smoothness or actual 50 MHz clock is
claimed: compiler/MMIO overhead and physical timing are unmeasured.

## Startup and watchdog boundary

The target uses **only** `lm3s6965_application_entry` for its one vector table
and inherited `Application_Reset_Handler`, plus the existing boot-startup
archive. It does not link the BSP OBJECT library (which carries a second
startup and the default linker). The sole slot linker is `linker/app_a.ld`,
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
PWM iterations are none of those predicates. This fixture does not fabricate
evidence, feed blindly, restart/reconfigure/disable the watchdog, reset a budget,
or claim a timer owned by boot. If later dispatched with a live watchdog,
expiry/reset is expected unless a separately reviewed owner integration exists;
the retained startup timer may also expire. Sustained physical breathing is
therefore **not accepted or hardware ready**, even if this payload links.
Correct continuity closure may substantially exceed 1 KiB; do not replace it
with cold startup to chase size. See [startup contract](../../docs/boot-startup.md).

## Retained offline outcome (2026-10-20)

The corrected gate completed **7 passing outcomes / 0 failures / 0 invalid,
five logical representatives**, not seven physical tests. Actual Release ELF,
canonical/BIN/HEX/PT_LOAD equality, standalone host PWM envelope assertions,
actual slot-A/release-42 signing, native selected-root MbedTLS acceptance and
two tamper rejections, and fresh boot final-image/root closure passed. The host
envelope is not physical LED behavior. Historical blocked producer and invalid
startup-oracle attempts remain separate; they are not erased by this result.

Application payload is **2784 bytes**, SHA256
`a02cbb060dc915b1f780b4c7e83185b0446f214784564c92164d80648e1a6a02`.
The retained `build/rnd-breathing-led/fixture-a-release42-corrected.pkg` is
**2944 bytes = manifest160 + payload2784**, SHA256
`2715d8d43074a8e9597d5b6de9c1c81bc026f8b949662db81410f79821f03ea0`.
Public identity `build/rnd-breathing-led/public.sec1` (65-byte raw SEC1) has SHA256
`960d06c1aaa1206cf37ce8e8b0cd002dfa441b9324e25c8c5a0735651f12c389`.
The matching private key remains in external custody; no private material is
part of this fixture.

The newly rebuilt selected-root boot is **31100 bytes**, still decision-only.
It is **not installed**: the board still has the older RFC-root boot, with no
new provisioning. No metadata has been generated. Future payload programming
would cover `[0x8800,0x92e0)`, erase `[0x8800,0x9400)` (**three** 1024-byte
pages); manifest160 would occupy `[0x23c00,0x23ca0)` in erase page
`[0x23c00,0x24000)`. New boot erase would be `[0,0x7c00)` (31 pages).
These are informational, not authorization. Future metadata generation and
execution need separate agreement covering exactly both metadata pages
`[0x8000,0x8400)` and `[0x8400,0x8800)`, including possible boot-time writes.
Transport `.pkg` must not be flashed as a contiguous slot image.

## Reference commands (not new execution authority)

The retained gate already ran; these are reproduction references only. Further
build/test execution belongs to separately authorized test-merit-gate:

```sh
cmake -S . -B /tmp/lm3s6965-breathing-led \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_BOOT_BREATHING_LED=ON
cmake --build /tmp/lm3s6965-breathing-led --target lm3s6965_boot_breathing_led
cc -std=c11 -Wall -Wextra -Werror examples/boot-breathing-led/test_pwm_envelope.c \
  -o /tmp/lm3s6965-pwm-envelope
/tmp/lm3s6965-pwm-envelope
```

Final ELF (no `.elf` suffix):
`/tmp/lm3s6965-breathing-led/examples/boot-breathing-led/lm3s6965_boot_breathing_led`.
The same directory receives `.map`, `.bin` and `.hex`. These outputs are not
signed packages. The fixture-local `generate-slot-a-images.py` validates the ELF
with the existing packager parser, then passes explicit `--only-section` arguments
to objcopy for nonzero allocated file-backed sections with FLASH LMA in
`[0x8800, 0x23c00)`, using `0xff` gap fill. Empty sections (especially the
zero-size SRAM `.data` that made unfiltered objcopy emit a roughly 512 MiB
binary) and NOBITS are excluded. Future nonempty `.data` is included by its
FLASH LMA, not its SRAM VMA. Nonempty loads outside the payload are rejected;
SRAM NOLOAD reservations remain valid. No shared helper or linker change is
needed. Canonical ELF/bin equivalence remains an acceptance gate; the retained corrected
run verified it independently for the exact artifacts above. This producer fix
is fixture-local: no shared objcopy helper or linker policy was changed.

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
elf = Path('/tmp/lm3s6965-breathing-led/examples/boot-breathing-led/lm3s6965_boot_breathing_led')
payload = canonical_payload(read_input(elf), 'A', 'elf')
print('Validated slot-A canonical payload bytes:', len(payload))
PY
```

The retained signing used that exact path as `--input`,
`--input-format elf --slot A --release 42`, the explicitly selected public root
and retained external key, per [packaging](../../docs/boot-package.md).
This documentation accesses no key and grants no new signing authority. Offline
signature acceptance is not dispatch, service-gap, clock, hardware or sustained-LED
acceptance. See also [decision-only boot](../../docs/boot-reset-image.md).
