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
> against the configured baud rate. See [Known limitations](../README.md#known-limitations).

### Why the divisor is off by one

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

### 50 MHz is the ceiling

Table 5-5 of the datasheet marks `SYSDIV` encodings `0x0`, `0x1`, and `0x2` as reserved while
`BYPASS = 0`. The smallest usable divisor is `/4`, so 50 MHz is the fastest the PLL can clock
the core. `DC1.MINSYSDIV = 0x3` enforces the same limit. Running faster would require sourcing
the system clock from somewhere other than the PLL.

### PLL control details

`PWRDN` is active-low. At reset it reads `1` — the PLL is powered down — and enabling the PLL
means **clearing** the bit. That 1 → 0 transition is what raises the lock interrupt. Code that
sets the bit gets the opposite of what it wants, and the lock poll then spins forever.

`XTAL` is an index into a fixed table of supported crystal frequencies, not a raw frequency
value. Index `14` means 8 MHz on this part. The LM3S-era table has 16 entries from 1 MHz to
8.192 MHz. A wrong index configures the PLL for the wrong reference, which usually means the
part does not lock.

The reset value of `RCC` is `0x078E3AC0`: `PWRDN` = 1, `BYPASS` = 1, and `USESYSDIV` = 0. The
system boots on the oscillator with the PLL bypassed.

### Register bit positions

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
[Conventions](conventions.md) for why the datasheet, not a family-wide header, is the authority
for whether a field exists on this part.
