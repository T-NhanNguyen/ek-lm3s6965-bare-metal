## Function index

| Function | Description |
|---|---|
| `main` (`examples/baremetal/main.c`) | Initializes UART0, the SWO trace unit, SysTick, the LED, and the switches. The function prints the banner, then blinks the LED when a switch is pressed. |
| `main` (`examples/freertos/main.c`) | Initializes UART0, the SWO trace unit, and the LED, creates the heartbeat and console tasks, and starts the scheduler. |
| `Reset_Handler` | Copies `.data` to SRAM, zeroes `.bss`, runs `__libc_init_array`, and calls `main`. |
| `Default_Handler` | Catches every unhandled interrupt and parks forever. |
| `_init` | Empty stub that `crti.o` normally supplies. The `-nostartfiles` flag omits it. |
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
| `_sbrk` | Bump allocator. The heap grows from `_end` toward the stack. |
| `_read`, `_close`, `_fstat`, `_isatty`, `_lseek`, `_exit`, `_kill`, `_getpid` | Remaining newlib stubs |
