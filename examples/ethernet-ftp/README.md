# Ethernet FTP: standalone and inherited foreground fixture

The standalone `lm3s6965_ethernet_ftp` and opt-in slot-linked
`lm3s6965_ethernet_ftp_foreground` are different startup contracts. The latter
uses authenticated inherited continuity, not cold-reset/debugger entry. Both
remain RAM-backed file examples; this fixture does not wire a production flash
updater. See [platform contract](PLATFORM_CONTRACT.md),
[transport validation](TRANSPORT_VALIDATION.md) and
[startup contract](../../docs/boot-startup.md).

## Corrected both-slot fixtures — bounded offline acceptance (2026-10-20)

Both new A and B images are `lm3s6965_ethernet_ftp_foreground`, with the same
RAM-backed FTP/network/watchdog foreground owner. Slot-local compilation selects
PF0 breathing for A and repeating **100 ms ON / 100 ms OFF / 100 ms ON /
500 ms OFF** for B. The historical standalone breathing example and retained
release42/43 packages below are unchanged; they are **not** the corrected pair.
Actual ignored corrected outputs are now generated in `build/rnd-ftp-led-pair/`:
`slot-a-release44.pkg`, `slot-b-release45.pkg`, `metadata-page0.bin` and
`metadata-page1.bin`. Both payloads are33532B, packages33692B. The real
selected-root verifier accepted both packages; authenticated real-encoder
metadata has sequence1/Acurrent/Bprevious/confirmed mask3, no candidate/intent,
zero accounting/trial IDs, page0valid168B plus FF tail and page1allFF.
Recorded gate: **7pass/0fail/0invalid**, five logical representatives/seven
configurations; image checks static and LED tests GPIO-mocked. No runtime
A-selection/B-fallback, physical LED/FTP or full inherited runtime acceptance
from that offline gate alone; the later physical A44 decision is recorded below.

The following build recipe is reference only, not new execution authority;
use fresh paths and explicit fresh labels for any separately authorized run.
For each `SLOT=A` and `SLOT=B`:

```sh
SLOT=A # repeat with B in a separate fresh build directory
cmake -S . -B "/tmp/lm3s6965-ftp-pattern-${SLOT}" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON \
  -DLM3S6965_FTP_FOREGROUND_OWNER=ON -DLM3S6965_FTP_FOREGROUND_SLOT="${SLOT}"
cmake --build "/tmp/lm3s6965-ftp-pattern-${SLOT}" \
  --target lm3s6965_ethernet_ftp_foreground
```

PF0 initialization is masked and precedes `net_clock_initialize`. The existing
1 kHz application SysTick adds only bounded LED state/GPIO work: A uses a
triangular 2000-tick brightness envelope and first-order pulse-density
accumulation (no foreground PWM jitter, extra timer or division); B counts an
800-tick cycle. Missed/coalesced interrupts stretch these patterns, never cause
catch-up. `sys_now` wrap is irrelevant to pattern state. No physical smoothness
or timing claim is made. PF0 masked GPIO writes preserve Ethernet PF2/PF3.
Standalone SysTick, clock ownership/transition, foreground progress reports and
watchdog feed policy are unchanged; the ISR performs no protocol/service/feed.

See [installed metadata outcome and exact ranges](../../docs/boot-fixture-metadata.md)
for selected-root identity, accepted pages and reference validation recipes.
The matching boot and both FTP A44/B45 payloads/manifests plus metadata are now
installed: exact101pages/seven writes/98652 bytes verified, metadata commit last.
One SYSRESETREQ/continue captured state7, successful RUN decision with real A44
verification, base0x8800/size33532 and matching payload hash. No manual feed/freeze
or dispatch occurred. Cleanup last confirmed halted, breakpoint removed and
owned server alive; no persistence claim. B fallback, physical LED/network,
retained startup/application initialization and watchdog timing remain pending.
The requested decision-and-stop increment is satisfied; bounded application
dispatch is next, not implemented here. This docs update performs no
build/signing/test/key/hardware operations or new authorization.

## Historical secondary slot-B build recipe (release43, no LED pattern)

From the repository root, with the existing toolchain and populated lwIP
submodule (no automatic fetch), for a later authorized offline build:

```sh
cmake -S . -B /tmp/lm3s6965-ftp-slot-b-rnd \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLM3S6965_BUILD_ETHERNET_FTP=ON \
  -DLM3S6965_FTP_FOREGROUND_OWNER=ON \
  -DLM3S6965_FTP_FOREGROUND_SLOT=B
cmake --build /tmp/lm3s6965-ftp-slot-b-rnd \
  --target lm3s6965_ethernet_ftp_foreground
```

The ELF has no suffix:
`/tmp/lm3s6965-ftp-slot-b-rnd/examples/ethernet-ftp/lm3s6965_ethernet_ftp_foreground`.
The foreground-only link selects `linker/app_b.ld`, GNU maximum page size
`0x100` and `--entry=Application_Reset_Handler`; standalone policy is unchanged.
Inspect actual final vectors, unique defined continuity entry, load LMAs,
retained SRAM and canonical/BIN/decoded HEX equivalence before packaging.
Do not transplant the breathing fixture's A-only image generator: B's retained
artifact has nonempty 8-byte initialized data at a FLASH LMA.

## Sign with the retained identity, not a new key

Reuse the already authorized external private key and matching raw65 public
file; do not generate replacements or copy private material into the checkout.
`RND_SIGNING_KEY` below must already name the absolute external custody path
(key0600, containing directory0700). This is an instruction, not new signing
authority. Choose an output that does not exist; the retained package must not
be overwritten or re-signed just for an ELF-entry metadata correction.

```sh
python3 tools/boot/package.py \
  --input /tmp/lm3s6965-ftp-slot-b-rnd/examples/ethernet-ftp/lm3s6965_ethernet_ftp_foreground \
  --input-format elf --slot B --release 43 \
  --public-key build/rnd-breathing-led/public.sec1 \
  --private-key "${RND_SIGNING_KEY:?set the authorized external key path}" \
  --output build/rnd-breathing-led/ethernet-ftp-slot-b-release43.pkg
```

The output's parent must exist. See [packaging](../../docs/boot-package.md) for
strict input/output checks and selected-root native validation requirements;
the default-demo native verifier cannot accept this different root.

## Historical retained offline inventory (2026-10-20; superseded by pair44/45)

Slot A is the [breathing fixture](../ab-boot-demo/slot-a-breathing/README.md): release42,
payload2784/package2944. Slot B is this existing foreground application:
release43, payload**33288**/package**33448**. Both use
`build/rnd-breathing-led/public.sec1`, SHA256
`960d06c1aaa1206cf37ce8e8b0cd002dfa441b9324e25c8c5a0735651f12c389`.
Retained B package `build/rnd-breathing-led/ethernet-ftp-slot-b-release43.pkg`
has SHA256 `e6c79dc74442e3c834360c4d06447cc93215a42930c434ffc97c4c3f0d3ea91c`.

Captured corrected B ELF e_entry = authenticated vector = defined continuity
FUNC `Application_Reset_Handler` at Thumb **0x24bdd**. Its foreground-only CMake
fix changed ELF/HEX start metadata, not the signed payload or package; no
re-signing occurred or is needed. Canonical/PT_LOAD/BIN/decoded HEX memory and
package payload remain equal, SHA256
`fe558f459d6fde3e1c6465cfaa8ab26d8ddf8422e3fa568755b86defe5e7f6d2`.

Original B gate: **7 passing outcomes / 0 failures / 0 invalid, five logical
representatives**, including selected-root signature/hash checks and real
native MbedTLS accept plus payload/signature tamper rejection (statuses8/4).
The entry follow-up is offline static adjudication, not fresh native tests;
its historical whole-HEX-hash guard stop remains0pass/0productfail/3invalid.
Two prebuilt modeled owner/clock checks passed in the original gate, but their
current-source freshness was not established. Neither these nor existing
standalone tests prove inherited full Ethernet/FTP/watchdog runtime.

Future B payload program `[0x24000,0x2c208)`, erase
`[0x24000,0x2c400)` (**33**1024-byte pages); manifest160 program
`[0x3f400,0x3f4a0)`, erase `[0x3f400,0x3f800)`. Informational only:
`.pkg` is transport manifest+payload, not a directly flashable contiguous image.
At this historical42/43 checkpoint metadata was not generated; the corrected
44/45 pages are now generated as recorded above. Any provisioning/decision plan must separately cover
both metadata pages `[0x8000,0x8400)` and `[0x8400,0x8800)`, including possible
boot-time writes. Board boot still has the older RFC root, not this public
identity. No new provisioning, board writes or dispatch occurred. Decision-only
boot cannot launch either fixture. Full physical/inherited runtime, timing,
watchdog service and resource peaks remain unaccepted. The current next boundary
is separately authorized physical installation, not autonomous dispatch.
