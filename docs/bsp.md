# BSP registers and conventions

The per-part datasheet defines register ownership. The BSP supplies small register headers and startup helpers rather than importing a family-wide SDK. Confirm both register semantics and board wiring before using a peripheral.

## Find the contract

- [Memory map](#memory-map)
- [Header conventions](#header-conventions)
- [Function index](#function-index)


## Memory map

| Region | Address | Size |
|---|---|---|
| Flash | `0x00000000` | 256 KB |
| SRAM | `0x20000000` | 64 KB |
| Peripherals | `0x40000000` |. |
| Bit-band alias (SRAM / peripheral) | `0x22000000` / `0x42000000` |. |
| Private peripheral bus (SysTick, NVIC) | `0xE0000000` |. |

The peripheral base addresses (UART0 `0x4000C000`, GPIO A to G, SSI, I2C, GPTM, ADC, and
Ethernet) are in `include/lm3s6965/memory_map.h`.


## Header conventions

The register headers follow a small set of rules for include guards, constant representation,
and datasheet-versus-header authority.

#### Include guards, not `#pragma once`

The headers use `#ifndef`/`#define` guards with a `LM3S6965_<FILE>_H` prefix. `#pragma once` is
not a free upgrade:

- Include guards are the ISO C mechanism, and GCC's multiple-include optimization means a
  guarded header is not rescanned (verified with `-H`), so there is no performance argument for
  `#pragma once`.
- `#pragma once` is non-standard (not in C11, C23, or C++23) and keys on filesystem identity,
  not a name. The same logical header can appear at two paths as build-stamped copies,
  generated headers, vendored duplicates, or a symlink into a build tree. If those paths are
  treated as distinct files, the header is processed twice, producing redefinition errors
  that a name-based guard would have prevented.

A guard macro's real risk is that it lives in the global macro namespace and can be clobbered,
so two headers that share a guard name silently skip the second. The unique project prefix
neutralises that risk.

#### `enum` versus `#define`

C is not C++: an `enum` enumerator is an integer constant expression, but a `const` variable is
not. A `const` cannot size an array, label a `case`, seed a `_Static_assert`, or set a bit-field
width. An enumerator can.

| Constant kind | Representation | Reason |
|---|---|---|
| Base addresses | `#define` | `0xE0000000`-class values exceed `INT_MAX`. Not representable in a C11 enum. |
| Register offsets (cohesive block) | `typedef enum` | Small ints, a genuine set, usable as constant expressions. |
| `*_SHIFT` positions (cohesive block) | `typedef enum` | Small ints. Masks may reference them because enumerators are constant expressions. |
| Bit masks (`*_MASK`, `*_BIT`, flags) | `#define` | Need unsigned `1u << n` type and full 32-bit width. |
| Counts (vector table, priority width) | `typedef enum` | Cohesive block of small ints. |
| Function-like generators | `#define` | `REGISTER32` and `RCC_SYSDIV_FOR_DIVISOR` cannot be enums. |
| `static const` in a header | never | Not a constant expression. Per-TU storage. `-Wunused-const-variable` hazard. |

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

#### The datasheet is the per-part authority

StellarisWare and TivaWare ship one header per family, not per part, so a family header defines
every bit field that exists on *any* member. Your part implements a subset. A field present in
the header can be absent on the chip, and a write to it can silently corrupt reserved bits.

Fields that are family fiction on the LM3S6965 include `SYSCTL_RCC_OEN`, `SYSCTL_RCC2_DIV400`,
`SYSCTL_RCC2_SYSDIV2LSB`, and the whole `SYSCTL_PLLSTAT` register. `PLLSTAT` is the dangerous
one: current TivaWare polls `SYSCTL_PLLSTAT.LOCK` for PLL lock, but that register does not exist
on this part, so copied code would read a non-existent address and spin forever. The LM3S
mechanism is `RIS.PLLLRIS` (bit 6). See [System clock](peripherals.md).

Before trusting a field from a vendor header:

1. Confirm the header targets your part, not just your family. A GitHub mirror with "tiva" or
   "stellaris" in the name proves nothing. A common mirror such as `yuvadm/tiva-c` is TivaWare
   for TM4C123, an entirely different part whose bit positions happen to match.
2. Grep the datasheet for the field name. Zero occurrences means it is not yours.
3. Check the reset value. A real field has a documented reset. Reserved bits have a
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


## Function index

| Function | Description |
|---|---|
| `main` (`examples/baremetal/main.c`) | Initializes UART0, the SWO trace unit, SysTick, the LED, and the switches. The function prints the banner, then blinks the LED when a switch is pressed. |
| `main` (`examples/freertos/main.c`) | Initializes UART0, the SWO trace unit, and the LED, creates the heartbeat and console tasks, and starts the scheduler. |
| `Reset_Handler` | Copies `.data` to SRAM, zeroes `.bss`, runs `__libc_init_array`, and calls `main`. |
| `Default_Handler` | Catches every unhandled interrupt and parks forever. |
| `_init` | Empty stub that `crti.o` normally supplies. The `-nostartfiles` flag omits it. |
| `ssi0_initialize` | Configures SSI0 as an 8-bit mode-3 master at the fastest rate that does not exceed the requested rate. |
| `ssi0_write_byte` | Drains unused RX data, waits for TX space, and writes one byte to SSI0. |
| `ssi0_wait_idle` | Drains unused RX data and waits until SSI0 completes the transfer. |
| `ssi0_enable` | Enables SSI0. |
| `ssi0_disable` | Disables SSI0. |
| `oled_initialize` | Configures panel pins, enables panel power, initializes SSI0, and sends the panel initialization sequence. |
| `oled_clear_screen` | Clears all 128 x 96 pixels to black and waits for completion. |
| `oled_draw_image` | Expands a full-width 1-bit image at row zero, writes black bottom rows, and waits for completion. |
| `gpio_configure_output` | Configures the masked port pins as digital GPIO outputs without pull resistors. |
| `uart0_initialize` | Configures the UART0 clock gate, the baud divisor, and 8N1 framing. |
| `uart0_write_byte` | Blocks until the TX FIFO has room. The function then writes one byte. |
| `uart0_write` | Writes a NUL-terminated string to UART0. |
| `uart0_register_address` | Maps a UART0 register offset to an absolute address. |
| `system_control_register_address` | Maps a SYSCTL register offset to an absolute address. |
| `gpio_register_address` | Maps a GPIO port register offset to an absolute address. |
| `gpio_select_alternate_function` | Routes a port's masked pins to their alternate hardware function (`GPIOAFSEL`). |
| `gpio_select_protected_alternate_function` | Same, for the guarded PB7/PC[3:0] pins: unlocks `GPIOLOCK`, sets the `GPIOCR` commit bits, writes `GPIOAFSEL`, then re-locks. |
| `gpio_enable_digital_function` | Enables the digital function on a port's masked pins (`GPIODEN`). |
| `gpio_configure_input` | Configures a port's masked pins as digital inputs with the selected pull resistor. |
| `gpio_read_pins` | Reads the masked pin levels through the `GPIODATA` address mask. |
| `gpio_enable_pull_up` | Enables the internal pull-up on a port's masked pins (`GPIOPUR`). |
| `gpio_enable_pull_down` | Enables the internal pull-down on a port's masked pins (`GPIOPDR`). |
| `user_led_initialize` | Configures the user LED pin as a digital output. |
| `user_led_write` | Turns the user LED on or off. |
| `user_switch_initialize` | Configures the navigation and select switchs as active-low inputs with internal pull-ups. |
| `user_switch_is_pressed` | Returns true while the selected switch pin reads low. |
| `trace_register_address` | Maps a trace register offset to an absolute address. |
| `trace_initialize` | Enables the ITM and TPIU so console output also leaves as SWO. |
| `trace_write_byte` | Waits for the ITM stimulus port ready bit, then writes one byte to port 0. |
| `systick_initialize` | Configures SysTick for 1 ms ticks from the system clock. |
| `delay_milliseconds` | Blocks for the given number of SysTick ticks. |
| `blink_status_led` | Blinks the user LED the given number of times. |
| `spin_delay` | Busy-waits a fixed number of loop iterations. |
| `create_demo_tasks` | Creates the heartbeat and console tasks for the FreeRTOS target. |
| `heartbeat_task` | Toggles the user LED every half period. |
| `console_task` | Prints a tick and message counter once per second. |
| `vApplicationMallocFailedHook` | Prints a message and halts when the FreeRTOS heap is exhausted. |
| `vApplicationStackOverflowHook` | Prints the task name and halts on a task stack overflow. |
| `freertos_assert_failed` | Prints the failing file and line and halts. |
| `system_control_enable_peripheral_clock` | Sets a clock-gating bit in a SYSCTL `RCGC` register. |
| `system_control_configure_pll` | Switches the core to the 50 MHz PLL output. Returns false and stays on the oscillator if the PLL never locks. |
| `system_control_wait_for_pll_lock` | Polls `RIS.PLLLRIS` for PLL lock with a bounded timeout. Returns the iterations remaining. |
| `system_control_read_rcc` | Reads back `RCC`. The firmware uses this value to check its own clock configuration. |
| `_write` | newlib stub that sends stdout to UART0 and to the ITM. |
| `_sbrk` | Bounded allocator backing from `_heap_static_end` through `_heap_end`. Static heap, continuity, diagnostics and stack retain separate ownership. |
| `_read`, `_close`, `_fstat`, `_isatty`, `_lseek`, `_exit`, `_kill`, `_getpid` | Remaining newlib stubs |
