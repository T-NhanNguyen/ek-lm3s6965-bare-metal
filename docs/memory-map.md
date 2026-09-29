## Memory map

| Region | Address | Size |
|---|---|---|
| Flash | `0x00000000` | 256 KB |
| SRAM | `0x20000000` | 64 KB |
| Peripherals | `0x40000000` | — |
| Bit-band alias (SRAM / peripheral) | `0x22000000` / `0x42000000` | — |
| Private peripheral bus (SysTick, NVIC) | `0xE0000000` | — |

The peripheral base addresses (UART0 `0x4000C000`, GPIO A to G, SSI, I2C, GPTM, ADC, and
Ethernet) are in `include/lm3s6965/memory_map.h`.
