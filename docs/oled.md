## OLED graphics display

The EK-LM3S6965 carries a 128 x 96 OLED panel. The panel uses 4-bit gray and connects to SSI0. The
panel datasheet is not in this repository, and the vendor no longer publishes it. This document
therefore recovers the protocol from board documents and from the closest reference driver. It
records the register and pin facts, the protocol, and the memory strategies.

Every fact here has a source. Facts marked **(unverified)** need a scope or a signal check on real
silicon.

### Panel and controller

| Item | Value | Source |
|---|---|---|
| Panel | RiT P14201 series | EVB user's guide, p13 |
| Resolution | 128 columns x 96 rows | EVB user's guide, p13 |
| Controller | SSD1329 | StellarisWare `rit128x96x4.c` comments |
| Gray scale | 4 bits per pixel, 16 levels | StellarisWare `rit128x96x4.c` |
| Interface | Synchronous serial (SSI), write-only | EVB user's guide, p14 |
| Reset | Wired to the MCU reset through the CPLD | EVB user's guide, p11 |

The SSD1329 datasheet was not available. All controller behavior in this document comes from the
StellarisWare EK-LM3S6965 driver, which is the vendor's own board support code.

### Wiring

Source: the EVB user's guide, Table 2-2 (p15), and the schematic on p18.

| MCU pin | Signal | EVB net | Direction | Role for the OLED |
|---|---|---|---|---|
| 28 | PA2 / SSI0CLK | `SSICLK` | out | panel serial clock (shared with microSD) |
| 31 | PA5 / SSI0TX | `SSITX` | out | panel data in (shared with microSD) |
| 29 | PA3 / SSI0FSS | `OLEDCSn` | out | panel chip select, active low |
| 22 | PC7 / PHB0 | `OLEDDC` | out | data/command select. Low = command, high = data. |
| 23 | PC6 / CCP3 | `EN+15V` | out | boost-converter enable. **High = panel powered.** |
| 30 | PA4 / SSI0RX | `SSIRX` | in | SD card data out. The OLED does not use it. |
| 10 | PD0 / IDX0 | `CARDCSn` | out | SD card chip select. The OLED does not use it. |

Three consequences follow:

1. **SSI0 is a shared bus.** The microSD card and the OLED share CLK and TX. Their chip selects
   are separate. The OLED uses `SSI0FSS`, and the SD card uses PD0, a plain GPIO. A future SD
   driver must not toggle `SSI0FSS` while it talks to the card. Otherwise the card traffic
   reaches the OLED. To keep the OLED deselected, leave PA3 as a GPIO output driven high.
2. **The panel has no reset GPIO.** It shares the MCU reset through the CPLD, so firmware never
   sequences reset.
3. **PC6 must be driven high.** Without it, the +15 V rail is off and the panel is dark. This
   holds no matter how correct the SSI traffic is. Check PC6 first when the panel shows nothing.

### Signal and framing facts

- **The SSI0 port is a master in Freescale SPI mode 3.** The mode uses SPO = 1 and SPH = 1, with
  8 data bits. Source: StellarisWare calls `SSIConfigSetExpClk(SSI0_BASE, ..., SSI_FRF_MOTO_MODE_3,
  SSI_MODE_MASTER, ulFrequency, 8)`. The SSI example in the LM3S6965 datasheet also uses SPO = 1
  and SPH = 1.
- **The chip select is the SSI frame signal.** PA3 is muxed to `SSI0FSS`, and the SSI peripheral
  asserts it around each frame. The driver does not bit-bang CS.
- **D/C is a plain GPIO.** PC7 is low for commands and command parameters, and high for pixel data. It
  must not change while the SSI is busy. The reference waits for the SSI busy bit to clear before
  each change. A driver must do the same, or it will tag a byte with the wrong type.

`src/ssi.c` drains the receive FIFO before each write and during the idle wait.
SSI has separate 8-deep TX and RX FIFOs. The transmit-only master receives a word
for every transmitted word, even though the OLED has no read path.
Without a drain, RX fills and stalls transfers in QEMU. The project has not
confirmed this stall on silicon. See [[ssi-rx-fifo-stall]].

### Recovered command set

The shipped driver keeps D/C low for commands and their parameters.
It sets D/C high for pixel data. There is no panel read path.

| Command | Parameters | Meaning |
|---|---|---|
| `0xFD` | `0x12` | unlock extended commands |
| `0xAE` / `0xAF` | - | display off / on |
| `0x94` | `0x00` | icon off |
| `0xA8` | rows-1 | multiplex ratio |
| `0x81` | contrast | contrast |
| `0x82` | current | pre-charge current |
| `0xA0` | remap byte | display remap. See below. |
| `0xA1` | line | display start line |
| `0xA2` | offset | display offset |
| `0xA4` | - | normal display mode |
| `0xB1` | `0x11` | phase length |
| `0xB2` | `0x23` | frame frequency |
| `0xB3` | `0xE2` | front clock divider |
| `0xB8` | 15 gray bytes | set gray-scale table |
| `0xBB` | period | second pre-charge period |
| `0xBC` | voltage | pre-charge voltage |
| `0x15` | start, end | set column window, in 2-pixel units (0-63 on this panel) |
| `0x75` | start, end | set row window (0-95 used of the controller's 0-127) |

The `0xA0` remap byte used by the reference is **`0x52`**, which selects horizontal address
increment. Its bits are these: bit 6 split-odd/even, bit 4 COM remap, bit 2 horizontal(0) or
vertical(1) increment, bit 1 nibble remap, and bit 0 column remap. **`0x56`** selects vertical
increment.

### Recovery init sequence

Source: the reference source array, re-parsed. The source stores a length byte that counts itself,
followed by the payload. The table shows the exact bytes the reference sends.

| # | Command | Parameters | Meaning |
|---|---|---|---|
| 1 | `0xFD` | `0x12` | unlock |
| 2 | `0xAE` | - | display off |
| 3 | `0x94` | `0x00` | icon off |
| 4 | `0xA8` | `0x5F` | multiplex ratio 95 (96 rows) |
| 5 | `0x81` | `0xB7` | contrast |
| 6 | `0x82` | `0x3F` | pre-charge current |
| 7 | `0xA0` | `0x52` | remap, horizontal increment |
| 8 | `0xA1` | `0x00` | start line 0 |
| 9 | `0xA2` | `0x00` | offset 0 |
| 10 | `0xA4` | - | normal mode |
| 11 | `0xB1` | `0x11` | phase length |
| 12 | `0xB2` | `0x23` | frame frequency |
| 13 | `0xB3` | `0xE2` | front clock divider |
| 14 | `0xB8` | `01 02 03 04 05 06 08 0A 0C 0E 10 13 16 1A 1E` | gray-scale table (15 bytes, gamma-shaped) |
| 15 | `0xBB` | `0x01` | second pre-charge period |
| 16 | `0xBC` | `0x3F` | pre-charge voltage |
| 17 | `0xAF` | - | display on |

The gray-table entry has 15 bytes, not the 16 bytes a plain 4-bit table suggests. The reference
pads each entry with a trailing `0xE3`, and its length convention excludes that byte. The table
shows what the vendor code sends. **(unverified)** whether the controller expects 15 or 16
entries. The reference works on the board, so treat 15 as correct until a scope shows otherwise.

### Pixel format

- The panel uses 4 bits per pixel and **two pixels per byte**. The **left pixel is in bits 7:4**
  and the right pixel is in bits 3:0. Source: the `RIT128x96x4ImageDraw` documentation.
- A full frame is `128 * 96 / 2 = 6144` bytes. The layout is row-major, with 64 bytes per row.
  Under horizontal increment, this is the exact order the controller reads for a full-screen
  window (`0x15 0 63`, then `0x75 0 95`).
- The window column address counts 2-pixel units. Therefore x coordinates are even, and the
  column bounds of a window are `x/2` to `(x + width - 2)/2`.

### Sources

- Stellaris LM3S6965 Evaluation Board user's guide, October 28 2008. `datasheets/Stellaris
  LM3S6965 Evaluation Board.pdf`: features p8, clocking and reset p11, OLED p13-14, Table 2-2 and
  schematic p15 and p18.
- Stellaris LM3S6965 Microcontroller Data Sheet, rev I, July 15 2014. `datasheets/LM3S6965
  Microcontroller Data Sheet.pdf`: SSI chapter 13 (register map p486, CR0 p488, CR1 p490, DR
  p492, SR p493, CPSR p495), RCGC1 p220.
- StellarisWare EK-LM3S6965 firmware package, revision 10636, `boards/ek-lm3s6965/drivers/
  rit128x96x4.c` and `.h`. The project consulted the mirror `github.com/VENGEL/StellarisWare`,
  path `StellarisWare/boards/ek-lm3s6965/drivers/`. This is TI-proprietary code, not open source.
  The project used it to recover interface facts only. The project copies no code, font table, or
  gamma table from it into this repository.
- QEMU `hw/arm/stellaris.c` (LM3S6965EVB machine) and `hw/display/ssd0323.c`.
- The project names these vendor references but does not have them: the RiT P14201 panel
  datasheet, the Solomon Systech SSD1329 controller datasheet, and the RiT P14201 application
  note.

### Verification caveats

- **The controller cannot be read.** There is no status or framebuffer readback. A primitive that
  must change one pixel needs the value of the neighboring pixel, or a shadow copy. This is the
  central design constraint.
- **QEMU models a different controller.** `qemu-system-arm -M lm3s6965evb` attaches a `ssd0323`
  (128 x 64, 1 bit per pixel) to SSI0, not an SSD1329. QEMU can prove that the SSI transport runs
  and that transfers complete. It cannot validate the recovered init sequence or the resulting
  image.
- **QEMU wires the OLED chip select to PD0, active high.** It raises the select at board
  creation. The board's real OLED select is `SSI0FSS`. An OLED-only guest needs no SD-card setup
  in QEMU.
- **QEMU ignores GPIO pin muxing.** On silicon, PA2, PA3, and PA5 must be muxed (`AFSEL = 1`) and
  digital-enabled (`DEN = 1`). PC6 and PC7 must be digital outputs. Emulation will not catch a
  missing mux. See [Peripherals](peripherals.md).
- **The panel clock ceiling is unverified.** The reference receives the frequency from the caller,
  and the quickstart uses a low rate. Treat 1 MHz as known-good, 4 MHz as a practical target, and
  8-10 MHz as a ceiling to confirm on hardware.

## Display memory strategies

The SSD1329 holds its own 6144-byte GDDRAM, and that memory *is* the frame buffer. The panel scans
it continuously. The design question is not "where is the frame". The question is "who owns a
mutable copy of it", because the panel copy cannot be read back.

People often discuss the two strategies below as alternatives. They are not. Window primitives are
the *transport*. A memory pool is an optional *composition* layer on top.

### Strategy A: window primitives

Set a window (`0x15`, `0x75`), select the increment direction, and stream bytes into it. No MCU
frame buffer is required.

- Good for clear, fill, rectangle, and a blit of an image whose bytes already exist, usually
  `const` data in flash.
- Weak for single-pixel plotting, lines, and circles. The controller cannot be read, so a change
  to one pixel inside a byte needs the value of the neighboring pixel. The usual fixes are three.
  Always write whole byte pairs, keep the small region under edit in RAM, or composite the whole
  frame in RAM first. The last fix is Strategy B.
- RAM cost: **0 bytes** for fills and for images that live in flash.
- Flash cost: **6144 bytes per immutable full-screen image**.

### Strategy B: pool of full-frame blocks

Reserve a fixed pool of equal-size blocks. Each block is exactly one frame (6144 bytes). A block
is a mutable, readable shadow of the panel. Draw into it with normal RAM writes, then blit it to
the panel with a single full-screen window.

- Good for pixel, line, and gauge drawing, for text composition, and for any animation that must
  build a frame before it shows the frame.
- Two blocks give a compose and show split. Render into one block while the other block is on the
  panel.
- **The panel itself is not double buffered.** A full-screen blit still overwrites GDDRAM while
  the panel scans it. A two-block pool removes *flicker and partial-update artifacts*, but it does
  not give true tear-free vsync. There is no frame-sync signal to wait on. To reduce tearing on a
  large update, make the update smaller. You cannot remove tearing.
- RAM cost: **N x 6144 bytes** plus a tiny allocator.

### Memory arithmetic

One frame:

```
128 columns x 96 rows x 4 bits = 49 152 bits = 6144 bytes = 6 KiB
```

The LM3S6965 has 65 536 bytes of SRAM. The project measured the current builds with
`arm-none-eabi-size`:

| Build | `.data` | `.bss` | Static SRAM | Of which FreeRTOS heap | Free after a 1 KiB stack reserve |
|---|---|---|---|---|---|
| `examples/baremetal` | 92 | 340 | 432 | - | 64 080 |
| `examples/freertos` | 96 | 33 460 | 33 556 | 32 768 (`ucHeap`) | 30 956 |

The linker script does not reserve stack. The stack grows down from `_estack`. The table assumes a
1 KiB reserve. New driver globals also come out of the "free" column.

Full-frame pool capacity at 6144 bytes per block:

| Blocks | Frame storage | Allocator (bitmap) | Total | % of 64 KiB | Bare-metal headroom | FreeRTOS headroom |
|---|---|---|---|---|---|---|
| 1 | 6 144 | 4 | 6 148 | 9.4 % | 57 932 | 24 808 |
| 2 | 12 288 | 4 | 12 292 | 18.8 % | 51 788 | 18 664 |
| 3 | 18 432 | 4 | 18 436 | 28.1 % | 45 644 | 12 520 |
| 4 | 24 576 | 4 | 24 580 | 37.5 % | 39 500 | 6 376 |
| 5 | 30 720 | 4 | 30 724 | 46.9 % | 33 356 | 232 (too tight) |
| 8 | 49 152 | 4 | 49 156 | 75.0 % | 14 924 | negative |
| 10 | 61 440 | 4 | 61 444 | 93.8 % | 2 636 (too tight) | negative |

Readings:

- **Bare-metal:** 8 frames is comfortable and 10 is the hard ceiling.
- **FreeRTOS:** 4 frames is the safe maximum with a dedicated static pool. 5 frames leaves only
  232 bytes of stack headroom, which is not safe.
- Two frames, the natural double-buffer size, costs **12 288 bytes, or 18.75 % of SRAM**.

Allocator choice: every block has the same size and blocks never coalesce. A free list is
therefore the wrong structure. A single `uint32_t` bitmask has one bit per block. It gives O(1)
acquire and release and needs 4 bytes for up to 32 blocks. It pairs directly with an enum handle:

```c
typedef enum { FRAME_NONE = -1, FRAME_0, FRAME_1, FRAME_2, FRAME_3, FRAME_COUNT } oled_frame_t;
static uint32_t s_frame_in_use;   /* bit n = FRAME_n allocated */
```

An intrusive free list would cost 4 bytes per block plus O(N) traversal, with no benefit at this
scale. A generic heap free list (for example FreeRTOS `heap_4`) is worse. It adds an 8-byte header
per allocation and fragments a heap that tasks also need. `heap_4` with the 32 KiB `ucHeap` fits
only 5 blocks (5 x 6152 = 30 760). That leaves about 2 KiB for task stacks, which the two demo
tasks already exceed. **A dedicated static pool is the right home for frame blocks, not the
general heap.**

### Allocator terminology: pool, free list, bitmask

These are two different layers, and they are easy to conflate:

- A **pool** is the *storage*. It is a reserved region cut into N equal-size blocks. A **frame
  pool** is a pool whose block size equals one full frame (6144 bytes).
- A **free list** is one *allocator implementation*. It tracks the free blocks as a linked list.
  Acquire pops the head and release pushes it back. The idiom "free-list pool" is real and valid.
- A **bitmask** is another allocator for the same pool. It uses one bit per block.

A frame pool is the structure. The free list and the bitmask are two ways to manage it. "Do not
use free lists" was shorthand for "do not use the linked-list *allocator* here", not for "do not
use a pool". Both manage the same pool:

| | Linked free list | Bitmask |
|---|---|---|
| Metadata | one pointer per block (4N bytes), or hidden in a union | one bit per block, 4 bytes total for N <= 32 |
| Acquire | pop head, O(1) | first zero bit, O(1) via `__builtin_ctz(~bits)` |
| Release | push head, O(1) | `bits &= ~(1u << n)`, O(1) |
| Double free / stale handle | silent list corruption | rejected by one mask test |
| Inspect state | walk the list | read one word |
| Variable block sizes | supported | not applicable |

Both are O(1). Equal-size blocks never coalesce, so the free list pointers buy nothing and its
failure modes are worse. The bitmask is the better fit. A free list (or a full allocator such as
`heap_4`) earns its keep only when block sizes vary or when blocks must coalesce. Frame buffers do
neither.

### Images versus frames

There are two lifetimes, and a confusion of the two wastes RAM:

| Kind | Storage | Cost | Mutability |
|---|---|---|---|
| Image asset (splash, logo, icon, menu) | flash, `const` | 6144 bytes **flash** each | immutable |
| Working frame (compose, animate, double-buffer) | SRAM pool | 6144 bytes **RAM** each | mutable |

An enum can index both. Each enum value is a *handle*. The pool owns the RAM and flash owns the
assets:

```c
typedef enum { IMG_SPLASH, IMG_LOGO, IMG_MENU, IMG_COUNT } oled_image_t;  /* flash */
typedef enum { FRAME_NONE = -1, FRAME_0, FRAME_1, FRAME_COUNT } oled_frame_t;  /* RAM pool */
```

A flash asset streams straight to a window and costs no SRAM. Only a frame that must be drawn
into, or shown while another is built, needs a pool block. Flash budget for context: the
bare-metal image is 5940 bytes. One full-screen asset roughly doubles the firmware size. The
256 KiB flash holds about 42 such assets with no code at all.

### Worked example: `include/assets/image_lumon_bits.h`

The repository holds one demo asset, a 1-bit-per-pixel bitmap:

| Property | Value |
|---|---|
| Source file | `include/assets/image_lumon_bits.h` |
| Array size | 1152 bytes |
| Dimensions | **128 wide x 72 tall** (16 bytes per row) |
| Format | 1 bit per pixel, row-major, MSB = leftmost pixel |
| Aspect | does **not** fill the 96-row panel. 24 rows are unused. |
| Compiler include path | `lm3s6965_headers` exports `include/assets`. The demo includes `image_lumon_bits.h`. |
| Dependency | No U8g2 dependency. The header uses a plain `static const unsigned char` array. |

The panel uses 4 bits per pixel, so the 1bpp asset cannot stream directly. Each source bit must
become a 4-bit nibble:

```
1bpp region:  128 x 72 / 8           = 1152 bytes   (flash)
4bpp region:  128 x 72 / 2           = 4608 bytes   (expanded)
blank bottom: 128 x 24 / 2           = 1536 bytes   (rows 72-95, level 0x0)
full frame:   128 x 96 / 2           = 6144 bytes
```

There are three ways to use the asset, and they map onto the strategies above:

| Approach | RAM | Flash | Notes |
|---|---|---|---|
| Expand on the fly while streaming | a few bytes | 1152 | window primitives, cheapest for a static image |
| Pre-expand to 4bpp in flash | 0 | 6144 | fastest to display, asset is 5.3x larger |
| Expand into a pooled frame | 6144 | 1152 | needed only to composite, overlay, or animate |

A plain full-frame image demo needs **no pool at all**. The pool enters only when the frame is
composed or animated. The asset now uses `static const unsigned char image_lumon_bits[]`
in flash. The driver maps source bit 1 to level `0xF` and source bit 0 to level `0x0`.
The image occupies rows 0-71. The driver writes 24 blank rows below it at level `0x0`.
The 128 x 72 LUMON image displays on the physical EK-LM3S6965 board.
The user confirmed the visible image.

### Runtime brightness

Command `0x81` sets contrast. It accepts `0x00`-`0xFF`.
The vendor default is `0xB7`. Use `0xE0` as the recommended ceiling.
This ceiling is not a verified electrical limit. High contrast increases current
and can reduce panel life.

Brightness range (experimental):
`0x40, 0x70, 0xA0, 0xB7, 0xC8, 0xD8, 0xE8, 0xF0, 0xFF`
This range is the safety limis, but still isn't bright enough

1. Let the firmware initialize SSI0 and the panel.
2. Stop any other debugger session.
3. Run `scripts/oled-brightness.sh <value>` from the repository root.
   Use a hex byte or decimal 0-255. For example, use `0xB7` to restore the default.
4. Check that the helper reports SSI0 idle. Use bright content to check the visible change.

The helper halts without a reset. It selects command mode through the PC7 masked
GPIO alias and sends `0x81` followed by the contrast byte with D/C low.
It leaves PC6 high, drains RX, waits for idle, and resumes firmware.
It does not change flash. A reset restores the firmware contrast default.
The helper leaves pre-charge current command `0x82` unchanged.
The panel has no readback. Black pixels remain dark at any contrast setting.
See [[oled-runtime-brightness-via-openocd]] for the register writes and test limits.

### Full-frame blit time

A full-screen update is 6144 bytes, or 49 152 bits, plus 8 command bytes:

| SSI clock | Full-frame time |
|---|---|
| 1 MHz | 49.2 ms |
| 2 MHz | 24.6 ms |
| 4 MHz | 12.3 ms |
| 8 MHz | 6.15 ms |
| 10 MHz | 4.92 ms |

Even at 4 MHz, a full frame takes 12.3 ms (about an 81 fps ceiling, before rendering). This is the
real reason to keep partial window updates available. A 6 x 8 text cell is 24 data bytes plus 8
command bytes, about 0.26 ms at 1 MHz.

### Decision guide

| Use case | Recommended strategy | RAM |
|---|---|---|
| Splash, logo, static screen | flash bitmap + full-window blit | 0 |
| Menus, status text, sensor readout | window primitives, per-cell writes | 0 to 24 B |
| Pixel/line/plot, gauges, composited UI | 1-block pool, blit on change | 6144 |
| Flicker-free animation | 2-block pool, compose then blit | 12 288 |
| Several resident mutable screens | N-block pool | N x 6144 |

The recommendation for this repository is a layered design. Use window primitives always. Add an
optional fixed-size frame pool with a bitmask allocator and enum handles. Default to two blocks
(double buffer) on bare metal and one block under FreeRTOS. Add a dirty-rectangle partial blit, so
not every update pays the full-frame cost.

### Chosen scope for the first milestone

| Decision | Choice |
|---|---|
| Runtime | bare metal first (`examples/`), FreeRTOS later |
| Demo home | inside `examples/`, alongside the bare-metal bring-up |
| microSD coexistence | out of scope. The display only. |
| First visual | the full 128 x 96 frame, using `include/assets/image_lumon_bits.h` |
| Text/font | deferred |
| Frame pool | deferred. The static image needs no pool. |

The first demo therefore initializes SSI0 and the panel, expands the 1152-byte 1bpp asset to 4bpp,
and streams it into a full-screen window. The image stays on screen. This tests the transport, the
init sequence, the window and addressing math, and the 1-to-4 bit expansion, with a few bytes of
RAM. A later milestone adds the pool when composition is needed.
