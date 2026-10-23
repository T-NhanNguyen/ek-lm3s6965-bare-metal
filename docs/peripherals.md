# Clocks, GPIO and OLED

Configure the clock before its consumers and enable each pin explicitly. QEMU does not validate pin muxing or the SSD1329 panel protocol. The OLED is write-only, so mutable pixel composition needs caller-owned shadow storage.

## Find the contract

- [System clock](#system-clock)
- [GPIO data and protected pins](#gpio-data-and-protected-pins)
- [OLED protocol and rendering](#oled-protocol-and-rendering)


## System clock

The LM3S6965 is a Fury-class part. Its PLL produces a fixed **200 MHz**. `SYSDIV` divides
that value down. The EK-LM3S6965 has an **8 MHz** crystal. 200 / 4 = **50 MHz**, so
`SYSDIV` holds `3`. The field encodes `divisor - 1`.

`system_control_configure_pll()` runs the TivaWare sequence:

1. Set `BYPASS` and clear `USESYSDIV`. The core stays clocked during the change.
2. Clear `MOSCDIS` to start the main oscillator. Wait for it to stabilize.
3. Select the crystal and the main oscillator. Use `XTAL` = 14 for 8 MHz, and `OSCSRC` =
   main oscillator.
4. Clear stale lock status in `MISC`. Then clear `PWRDN` to power up the PLL.
5. Wait for `RIS.PLLLRIS` (bit 6). This wait has a timeout.
6. Set `SYSDIV` and `USESYSDIV`. Then clear `BYPASS` to give the core to the PLL.

The order is important. If you clear `BYPASS` before the PLL locks, the core loses its
clock.

Check the `rcc:` line in the banner. It must read **`0x01CE1380`**. That value means
`SYSDIV`=3, `USESYSDIV`=1, `PWRDN`=0, `BYPASS`=0, `XTAL`=14, and `OSCSRC`=main.

> **What QEMU proves, and what it does not.** QEMU models `RCC` and `RCC2`. It computes the
> system clock from `SYSDIV`. It raises the PLL lock status bit. QEMU therefore proves that
> the configuration writes land and that the lock wait ends. QEMU does **not** time the UART
> against the configured baud rate. See [Known limitations](limitations.md).

#### Opt-in foreground clock continuity

The PLL sequence above describes cold standalone initialization. The opt-in
foreground application inherits validated RCC/RCC2 and system Hz from boot and
**does not rerun the PLL routine or change system frequency**. After RAM/core
initialization and FTP core reset, masked `boot_startup_c_ready` validates
readiness. `net_clock_initialize` starts application 1 kHz SysTick while masked,
then takes the final startup GPTM sample and publishes a separate conservative
entry-relative offset before safe unmask. LwIP/netif initialization precedes
explicit named GPTM release. No GPTM reads occur afterward. SysTick ISR time is
modular application milliseconds, with no WDT feed. This changes timer ownership,
not WDT frequency/load/history or the system clock. See
[startup continuity](boot-startup.md#startup-only-gptm--application-systick) for
APIs, offset arithmetic, provenance and bounded verification limits. Production
physical WDT/boot producer wiring and physical timing remain unproven.

#### Why the divisor is off by one

The `SYSDIV` field encodes `divisor - 1`, so 50 MHz needs `3`, not `4`. TivaWare's
configuration constants are named for the human-facing divisor while the register field holds
the decremented value. `SYSCTL_SYSDIV_4` is named for the divisor, but its value `0x01C00000`
encodes a field value of `3`:

```
SYSCTL_SYSDIV_4 = 0x01C00000
                  SYSDIV    = (0x01C00000 & 0x07800000) >> 23 = 3
                  USESYSDIV =  0x01C00000 & 0x00400000       = 1
```

Divisor 1 is a special case. `SYSCTL_SYSDIV_1 = 0x07800000` parks the field at its maximum
value `15` and **clears** `USESYSDIV`. Reading the field alone would suggest 200/16 MHz. The
divider is disabled, not set to 16, so always check `USESYSDIV` before believing `SYSDIV`.

#### 50 MHz is the ceiling

Table 5-5 of the datasheet marks `SYSDIV` encodings `0x0`, `0x1`, and `0x2` as reserved while
`BYPASS = 0`. The smallest usable divisor is `/4`, so 50 MHz is the fastest the PLL can clock
the core. `DC1.MINSYSDIV = 0x3` enforces the same limit. Running faster would require sourcing
the system clock from somewhere other than the PLL.

#### PLL control details

`PWRDN` is active-low. At reset it reads `1`. The PLL is powered down. And enabling the PLL
means **clearing** the bit. That 1 → 0 transition is what raises the lock interrupt. Code that
sets the bit gets the opposite of what it wants, and the lock poll then spins forever.

`XTAL` is an index into a fixed table of supported crystal frequencies, not a raw frequency
value. Index `14` means 8 MHz on this part. The LM3S-era table has 16 entries from 1 MHz to
8.192 MHz. A wrong index configures the PLL for the wrong reference, which usually means the
part does not lock.

The reset value of `RCC` is `0x078E3AC0`: `PWRDN` = 1, `BYPASS` = 1, and `USESYSDIV` = 0. The
system boots on the oscillator with the PLL bypassed.

#### Register bit positions

Two independent sources confirm the register bit positions: the LM3S/Fury StellarisWare
`hw_sysctl.h` header and the QEMU `hw/arm/stellaris.c` model.

| Field | TI `hw_sysctl.h` | QEMU `hw/arm/stellaris.c` |
|---|---|---|
| `XTAL` position | `SYSCTL_RCC_XTAL_M = 0x7C0` (bits 10:6) | `(rcc >> 6) & 0xf` |
| 8 MHz encoding | `SYSCTL_RCC_XTAL_8MHZ = 0x380` (field 14) | `pllcfg_fury[14] /* 8 Mhz */` |
| `SYSDIV` position | `SYSCTL_RCC_SYSDIV_M = 0x07800000`, shift 23 | `(rcc >> 23) & 0xf` |
| `PWRDN` position | `SYSCTL_RCC_PWRDN = 0x2000` (bit 13) | `rcc & (1 << 13)` |
| PLL-lock status | `SYSCTL_RIS_PLLLRIS = 0x40` (bit 6) | `int_status \|= (1 << 6)` |

Use the LM3S/Fury StellarisWare header, not TivaWare, which targets TM4C123 only. See
[Conventions](bsp.md) for why the datasheet, not a family-wide header, is the authority
for whether a field exists on this part.


## GPIO data and protected pins

The GPIO block drives the LED and the switches, and it guards the five JTAG/SWD pins behind a
lock-and-commit register pair.

#### The `GPIODATA` address mask

`GPIODATA` is read and written through an address mask, unlike every other GPIO register. The
datasheet states that bits which are `1` in the address mask cause the corresponding bits in
`GPIODATA` to be read, and bits that are `0` cause the corresponding bits to be read as `0`,
regardless of their value.

A read at the plain offset `base + 0x000` therefore applies a mask of zero and returns zero for
every pin, under every pull setting. The failure is silent: no bus fault and no status bit
reports it. The LED write path hides the fault because it uses the masked address
`base + (mask << 2)`.

Only `GPIODATA` uses the address mask. The configuration registers. `GPIODIR`, `GPIODEN`,
`GPIOPUR`, `GPIOPDR`, and `GPIOAFSEL`. Use their plain offsets.

Read the data register through the mask:

```c
uint32_t gpio_read_pins(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t masked_data_address =
        port_base_address + (pin_mask << GPIO_ADDRESS_MASK_SHIFT);

    return REGISTER32(masked_data_address);
}
```

#### The three-pull input scan

An input that never reports a press can be a wiring fault or a software fault. The three-pull
scan separates them: configure the pins as inputs and read them with `GPIO_PULL_UP`, again with
`GPIO_PULL_DOWN`, and again with `GPIO_PULL_DISABLED`, then print the three values. Repeat every
250 ms and press each switch during the capture.

A working active-low input drops its bit in the pull-up column. The Up switch changes the four
direction pins from `0xF` to `0xE`. A dead readback shows the same value in every column and for
every press, which points at the read path rather than the switch.

The scan changes the pull setting and reads in the same instant. The switch net has some
capacitance, so a pin can show the previous setting on the next read and the columns can shift
by one. If the columns look wrong, add a short delay after each pull change before reading.

#### Guarded debug pins: `GPIOLOCK` and `GPIOCR`

Five pins are write-protected: `PB7` and `PC[3:0]`, the JTAG/SWD pins. On every other pin
`GPIOAFSEL` can be written directly. On these five the write is silently discarded. No bus
fault, no status bit. And the register keeps its old value.

Two registers govern this:

| Register | Offset | Purpose | Reset |
|---|---|---|---|
| `GPIOLOCK` | `0x520` | The keyhole. Write `0x1ACCE551` to unlock `GPIOCR`. Any other value re-locks it. Reads return status: `0x1` locked, `0x0` unlocked. | `0x00000001` (locked) |
| `GPIOCR` | `0x524` | The gate. One bit per pin: `1` lets a write to the matching `GPIOAFSEL` bit commit, `0` blocks it. Writable only while `GPIOLOCK` is unlocked. | see below |

The sequence is: **unlock, set the commit bit, write `AFSEL`, re-lock.**

Measured reset values on the EK-LM3S6965:

| Port | `GPIOLOCK` | `GPIOCR` | Why |
|---|---|---|---|
| A, D, E, F, G | `0x00000001` | `0x000000FF` | no protected pins. All `CR` bits hardwired to `1` |
| B | `0x00000001` | `0x0000007F` | `PB7` not committable |
| C | `0x00000001` | `0x000000F0` | `PC[3:0]` not committable |

`GPIOAFSEL` resets to `0x0000000F` for Port C and `0x00000080` for Port B, because those five
pins come out of reset already owned by the debug hardware. The other `GPIOCR` bits are
hardwired to `1` and cannot be written with `0`. Only the five debug pins are gateable. The
register type is `RO` for every other pin.

Re-locking does not clear the commit bits. `GPIOCR` keeps whatever was last written, so if the
gate was taken down it must be put back up before re-locking, or the pin stays committable.

> Setting the commit bit and then `AFSEL` for `PC[3:0]` reclaims the SWD/JTAG port as GPIO.
> OpenOCD loses the target mid-session. A chip reset restores the debug port, but if the
> firmware does this at boot, every reset re-enters the same state and the debugger can never
> attach. Recovery then needs the ROM boot loader or a mass erase. Do not test this path on a
> board you need.

The mechanism can be proven without touching `AFSEL`, because `GPIOCR` only gates future
`AFSEL` writes and changes no pin:

```
mww 0x40004520 1          # GPIOLOCK A  -> 1  LOCKED
mww 0x40006524 0xFF       # write GPIOCR C while locked  -> discarded
mdw 0x40006524 1          #            -> 0xF0  (unchanged: gate held shut)
mww 0x40006520 0x1ACCE551 # unlock
mdw 0x40006520 1          #            -> 0     UNLOCKED
mww 0x40006524 0xFF       # same write, now commits
mdw 0x40006524 1          #            -> 0xFF
mww 0x40006524 0xF0       # restore protection
mww 0x40006520 0           # re-lock
```

The port's clock gate in `RCGC2` must be on first. Reading a GPIO block whose clock gate is off
gives `JTAG-DP STICKY ERROR`, not a muxing answer. This firmware enables only `GPIOA`
(`RCGC2 = 0x1`), so probing Port B or C needs `mww 0x400FE108 0x7` first, restored afterwards.

`gpio_select_protected_alternate_function()` in `src/gpio.c` performs the whole scoped sequence
and always re-locks before returning, so no caller can leave a port unlocked. It leaves the
commit bit set, because the pin has deliberately been converted. The lock, which is the
protection that matters, is restored. `gpio_select_alternate_function()` remains the plain path
and is wrong for these five pins. Unused functions are dropped by `--gc-sections`, so shipping
the helper costs zero bytes until something calls it.

See [Run without hardware](applications.md) for why emulation never catches a missing pin mux.


## OLED protocol and rendering

The EK-LM3S6965 carries a 128 x 96 OLED panel. The panel uses 4-bit gray and connects to SSI0. The
panel datasheet is not in this repository, and the vendor no longer publishes it. This document
therefore recovers the protocol from board documents and from the closest reference driver. It
records the register and pin facts, the protocol, and the memory strategies.

Every fact here has a source. Facts marked **(unverified)** need a scope or a signal check on real
silicon.

#### Panel and controller

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

#### Wiring

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

#### Signal and framing facts

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
confirmed this stall on silicon. See [emulation limits](applications.md#gpio-pin-muxing-is-not-modeled).

#### Recovered command set

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

#### Recovery init sequence

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

#### Pixel format

- The panel uses 4 bits per pixel and **two pixels per byte**. The **left pixel is in bits 7:4**
  and the right pixel is in bits 3:0. Source: the `RIT128x96x4ImageDraw` documentation.
- A full frame is `128 * 96 / 2 = 6144` bytes. The layout is row-major, with 64 bytes per row.
  Under horizontal increment, this is the exact order the controller reads for a full-screen
  window (`0x15 0 63`, then `0x75 0 95`).
- The window column address counts 2-pixel units. Therefore x coordinates are even, and the
  column bounds of a window are `x/2` to `(x + width - 2)/2`.

#### Sources

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

#### Verification caveats

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

### Display memory strategies

The SSD1329 holds its own 6144-byte GDDRAM, and that memory *is* the frame buffer. The panel scans
it continuously. The design question is not "where is the frame". The question is "who owns a
mutable copy of it", because the panel copy cannot be read back.

People often discuss the two strategies below as alternatives. They are not. Window primitives are
the *transport*. A memory pool is an optional *composition* layer on top.

#### Strategy A: window primitives

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

#### Strategy B: pool of full-frame blocks

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

#### Frame pool capacity

One mutable frame costs 6144 bytes. Two frames cost 12288 bytes, or 18.75 percent
of the device's 65536-byte SRAM, before allocator metadata. A bitmask allocator
costs four bytes for up to 32 blocks. Immutable image assets belong in flash and
need no frame allocation unless the application composites or edits them.

Use the final linker map for capacity decisions. Current linker contracts reserve
8192 stack bytes and 64 diagnostic bytes. Boot/A/B reserve another 1024 continuity
bytes. FreeRTOS also owns its configured 32768-byte heap inside BSS. Do not add
that heap twice, assume a former 1 KiB stack estimate or treat unused static
capacity as measured runtime headroom. See [memory ownership](boot-memory-layout.md).

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

#### Images versus frames

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

#### Worked example: `include/assets/image_lumon_bits.h`

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

#### Runtime brightness

Command `0x81` sets contrast in the recovered SSD1329 driver. The existing
`scripts/oled-brightness.sh` helper supports `0x00`-`0xFF` as a **byte test range**.
this is not a newly datasheet-verified SSD1329 parameter range or a safe operating range.
The TI driver initializes contrast to `0xB7`, citing the RiT P14201 application note.
Treat this as the vendor baseline, **not a panel-lifetime guarantee**. Neither `0xE0`
nor `0xFF` is a verified safe ceiling. Higher contrast can increase panel current and
reduce panel life. Brightness alone cannot establish electrical safety.

For comparison only, the supplied `datasheets/SSD0323_1-6.pdf` is Solomon Systech
**SSD0323 Rev. 1.6, October 2006**, a different 128 x 80 controller, not the board's
SSD1329/RiT P14201. Its command table and contrast description give `0x81` a
`0x00`-`0x7F` range (printed pp23, 29). It also defines separate current controls
`0x84`, `0x85`, and `0x86` (pp23, 29). Its electrical-characteristics table specifies
IREF = 10 microamps and a conditional segment-current typical value of 300 microamps
at contrast `0x7F` (p37). These are comparative evidence, **not transferable SSD1329
limits** or a basis for recommending higher settings on this panel.

Preview a single value without OpenOCD or hardware (decimal `183` is equivalent):

```bash
scripts/oled-brightness.sh --dry-run 0xB7
```

This validates the byte and prints the intended SWD/SSI operation without changing the
panel. The above-baseline warning still applies. Use `--help` for usage.

For an actual write:

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
The helper has no panel readback, so a successful command does not verify the resulting contrast.

##### Interactive contrast test

`scripts/oled-contrast-test.sh` reuses `oled-brightness.sh`. It does not implement a
second MMIO path. It has **no default sweep** and no unattended mode. All values are
validated before any hardware operation. Use decimal or `0x`-prefixed hex bytes.
range STEP is a positive magnitude (1-255), ascending or descending as needed. The
end value is included only if the step reaches it exactly. An explicit list preserves
order and duplicates.

```bash
# Preview only: NO hardware operations, including no restoration write.
scripts/oled-contrast-test.sh --dry-run 0x70 0xB7 0x10
scripts/oled-contrast-test.sh --dry-run --list 64 0x70 0xA0 0xB7

# Real test: terminal input and an explicit 'yes' before EVERY step.
scripts/oled-contrast-test.sh --list 0x70 0xA0 0xB7
scripts/oled-contrast-test.sh 0xB7 0x70 0x10
scripts/oled-contrast-test.sh --help
```

Prerequisites are the same as for the single-value helper: an already flashed and
running image that initialized the panel and SSI0, OpenOCD on PATH, the board/probe
connected, and no competing debugger. The test script does not build, flash, render
content, or diagnose darkness. Black pixels remain black. Use existing bright content
for visual comparisons. A warning precedes approval of every value above `0xB7`.
Do not infer safety from a successful write or a visible image.

Inspect the panel after each step. Any response other than exactly `yes`, EOF, or
Ctrl-C stops the sequence. After any real write attempt, the script attempts to restore
`0xB7` on completion, cancellation, caught interruption (INT/TERM/HUP), or helper
failure, even if that helper may have only partially written. Cancellation before the
first write does not touch hardware. Restoration is to the vendor baseline, not the
unknown prior setting. A restoration failure produces a prominent warning and a
nonzero exit status. The panel state is then unknown. There is no readback to verify
restoration, and power loss, forced termination (SIGKILL), or a lost probe connection
can prevent it. Check the board before continuing. Resetting an image configured with
`0xB7` reinitializes that baseline, but is not a safety or lifetime guarantee.

#### Full-frame blit time

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

#### Decision guide

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
