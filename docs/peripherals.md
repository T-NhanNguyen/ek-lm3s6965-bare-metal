## Peripherals

The GPIO block drives the LED and the switches, and it guards the five JTAG/SWD pins behind a
lock-and-commit register pair.

### The `GPIODATA` address mask

`GPIODATA` is read and written through an address mask, unlike every other GPIO register. The
datasheet states that bits which are `1` in the address mask cause the corresponding bits in
`GPIODATA` to be read, and bits that are `0` cause the corresponding bits to be read as `0`,
regardless of their value.

A read at the plain offset `base + 0x000` therefore applies a mask of zero and returns zero for
every pin, under every pull setting. The failure is silent: no bus fault and no status bit
reports it. The LED write path hides the fault because it uses the masked address
`base + (mask << 2)`.

Only `GPIODATA` uses the address mask. The configuration registers — `GPIODIR`, `GPIODEN`,
`GPIOPUR`, `GPIOPDR`, and `GPIOAFSEL` — use their plain offsets.

Read the data register through the mask:

```c
uint32_t gpio_read_pins(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t masked_data_address =
        port_base_address + (pin_mask << GPIO_ADDRESS_MASK_SHIFT);

    return REGISTER32(masked_data_address);
}
```

### The three-pull input scan

An input that never reports a press can be a wiring fault or a software fault. The three-pull
scan separates them: configure the pins as inputs and read them with `GPIO_PULL_UP`, again with
`GPIO_PULL_DOWN`, and again with `GPIO_PULL_DISABLED`, then print the three values. Repeat every
250 ms and press each switch during the capture.

A working active-low input drops its bit in the pull-up column — the Up switch changes the four
direction pins from `0xF` to `0xE`. A dead readback shows the same value in every column and for
every press, which points at the read path rather than the switch.

The scan changes the pull setting and reads in the same instant. The switch net has some
capacitance, so a pin can show the previous setting on the next read and the columns can shift
by one. If the columns look wrong, add a short delay after each pull change before reading.

### Guarded debug pins: `GPIOLOCK` and `GPIOCR`

Five pins are write-protected: `PB7` and `PC[3:0]`, the JTAG/SWD pins. On every other pin
`GPIOAFSEL` can be written directly. On these five the write is silently discarded — no bus
fault, no status bit — and the register keeps its old value.

Two registers govern this:

| Register | Offset | Purpose | Reset |
|---|---|---|---|
| `GPIOLOCK` | `0x520` | The keyhole. Write `0x1ACCE551` to unlock `GPIOCR`; any other value re-locks it. Reads return status: `0x1` locked, `0x0` unlocked. | `0x00000001` (locked) |
| `GPIOCR` | `0x524` | The gate. One bit per pin: `1` lets a write to the matching `GPIOAFSEL` bit commit, `0` blocks it. Writable only while `GPIOLOCK` is unlocked. | see below |

The sequence is: **unlock, set the commit bit, write `AFSEL`, re-lock.**

Measured reset values on the EK-LM3S6965:

| Port | `GPIOLOCK` | `GPIOCR` | Why |
|---|---|---|---|
| A, D, E, F, G | `0x00000001` | `0x000000FF` | no protected pins; all `CR` bits hardwired to `1` |
| B | `0x00000001` | `0x0000007F` | `PB7` not committable |
| C | `0x00000001` | `0x000000F0` | `PC[3:0]` not committable |

`GPIOAFSEL` resets to `0x0000000F` for Port C and `0x00000080` for Port B, because those five
pins come out of reset already owned by the debug hardware. The other `GPIOCR` bits are
hardwired to `1` and cannot be written with `0`; only the five debug pins are gateable. The
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
commit bit set, because the pin has deliberately been converted; the lock, which is the
protection that matters, is restored. `gpio_select_alternate_function()` remains the plain path
and is wrong for these five pins. Unused functions are dropped by `--gc-sections`, so shipping
the helper costs zero bytes until something calls it.

See [Run without hardware](qemu.md) for why emulation never catches a missing pin mux.
