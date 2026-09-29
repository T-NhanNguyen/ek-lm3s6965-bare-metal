## Conventions

The register headers follow a small set of rules for include guards, constant representation,
and datasheet-versus-header authority.

### Include guards, not `#pragma once`

The headers use `#ifndef`/`#define` guards with a `LM3S6965_<FILE>_H` prefix. `#pragma once` is
not a free upgrade:

- Include guards are the ISO C mechanism, and GCC's multiple-include optimization means a
  guarded header is not rescanned (verified with `-H`), so there is no performance argument for
  `#pragma once`.
- `#pragma once` is non-standard (not in C11, C23, or C++23) and keys on filesystem identity,
  not a name. The same logical header at two paths — build-stamped copies, generated headers,
  vendored duplicates, a symlink into a build tree — is processed twice, producing redefinition
  errors that a name-based guard would have prevented.

A guard macro's real risk is that it lives in the global macro namespace and can be clobbered,
so two headers that share a guard name silently skip the second. The unique project prefix
neutralises that risk.

### `enum` versus `#define`

C is not C++: an `enum` enumerator is an integer constant expression, but a `const` variable is
not. A `const` cannot size an array, label a `case`, seed a `_Static_assert`, or set a bit-field
width; an enumerator can.

| Constant kind | Representation | Reason |
|---|---|---|
| Base addresses | `#define` | `0xE0000000`-class values exceed `INT_MAX`; not representable in a C11 enum. |
| Register offsets (cohesive block) | `typedef enum` | Small ints, a genuine set, usable as constant expressions. |
| `*_SHIFT` positions (cohesive block) | `typedef enum` | Small ints; masks may reference them because enumerators are constant expressions. |
| Bit masks (`*_MASK`, `*_BIT`, flags) | `#define` | Need unsigned `1u << n` type and full 32-bit width. |
| Counts (vector table, priority width) | `typedef enum` | Cohesive block of small ints. |
| Function-like generators | `#define` | `REGISTER32` and `RCC_SYSDIV_FOR_DIVISOR` cannot be enums. |
| `static const` in a header | never | Not a constant expression; per-TU storage; `-Wunused-const-variable` hazard. |

Enumerators are `int` in C11, so values at or above `0x80000000` are out of range:
`PRIVATE_PERIPHERAL_BASE_ADDRESS = 0xE0000000`, `SYSTICK_BASE_ADDRESS = 0xE000E010`, and
`NVIC_BASE_ADDRESS = 0xE000E100` must stay `#define`d. Enumerators are signed, so they cannot
carry the unsigned type of a `1u << n` mask. Converting only *some* entries of a mixed
offsets/shifts/masks list breaks the visual cohesion for no correctness gain, so such a block is
converted whole or left as `#define`.

Applied here: `include/lm3s6965/uart.h` uses `uart_register_offset_t`, and
`include/lm3s6965/interrupts.h` uses `lm3s6965_interrupt_count_t`, with
`LM3S6965_VECTOR_TABLE_ENTRIES` defined in terms of the counts above it.
`include/lm3s6965/system_control.h` has a mixed list and stays `#define`.
`include/lm3s6965/memory_map.h` keeps addresses and sizes together as `#define`.

The change was representation-only: all translation units build with the project's exact flags
(`-Wall -Wextra -Werror -Os`), the image size was unchanged, and QEMU printed the full banner
with the `rcc: 0x01CE1380` self-check.

### The datasheet is the per-part authority

StellarisWare and TivaWare ship one header per family, not per part, so a family header defines
every bit field that exists on *any* member. Your part implements a subset. A field present in
the header can be absent on the chip, and a write to it can silently corrupt reserved bits.

Fields that are family fiction on the LM3S6965 include `SYSCTL_RCC_OEN`, `SYSCTL_RCC2_DIV400`,
`SYSCTL_RCC2_SYSDIV2LSB`, and the whole `SYSCTL_PLLSTAT` register. `PLLSTAT` is the dangerous
one: current TivaWare polls `SYSCTL_PLLSTAT.LOCK` for PLL lock, but that register does not exist
on this part, so copied code would read a non-existent address and spin forever. The LM3S
mechanism is `RIS.PLLLRIS` (bit 6). See [System clock](system-clock.md).

Before trusting a field from a vendor header:

1. Confirm the header targets your part, not just your family. A GitHub mirror with "tiva" or
   "stellaris" in the name proves nothing — a common mirror such as `yuvadm/tiva-c` is TivaWare
   for TM4C123, an entirely different part whose bit positions happen to match.
2. Grep the datasheet for the field name. Zero occurrences means it is not yours.
3. Check the reset value. A real field has a documented reset; reserved bits have a
   preserve-on-write rule instead.

Reserved bits are not spare storage. The datasheet requires their value to be preserved across
a read-modify-write, so always read-modify-write (`rcc & ~MASK | VALUE`) rather than blind-write.
That is why the `RCC` readback `0x01CE1380` still carries bit 12 set from the reset value.

```bash
# fast triage: which candidate fields actually exist on this part?
for f in OEN OFFC DIV400 SYSDIV2LSB PLLSTAT MINSYSDIV; do
  printf '%-12s %s\n' "$f" "$(grep -c "$f" datasheet.txt)"
done
```
