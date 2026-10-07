# FTP platform integration contract (offline source complete)

## Ownership and build

`LM3S6965_BUILD_ETHERNET_FTP` defaults OFF. Only the guarded examples
subdirectory references lwIP; existing targets do not depend on it. No dependency
fetch is performed. Inspected populated pin: lwIP 2.2.1,
`77dcd25a72509eb83f72b033d219b1d40cd8eb95`. Upstream `Filelists.cmake` is deliberately
not included because it generates files inside the submodule.

The ON configuration builds `lm3s6965_ftp_lwip` and
`lm3s6965_ftp_platform` OBJECT libraries plus the real `lm3s6965_ethernet_ftp`
executable. It links those objects and the reusable core, RAM, and TCP archives.
Sources are `src/ftp_core.c`, `src/ram_file.c`, and `src/ftp_tcp.c`.
The TCP factory consumes the lwIP target's public usage requirements.
See the [library guide](../../docs/ftp-library.md) for caller-config reuse.
`main.c` owns the immutable `hello` basename, address policy, and two 4096-byte buffers.
It initializes RAM, binds the core, then explicitly resets for cold boot before networking.
First start and link-down/restart never clear the bound file.
Final independent verification built the executable with no undefined symbols.
Earlier object-only and unresolved-transport statements are superseded.

The FTP link selects only BSP startup, system_control, gpio, and ethernet sources;
it does not link `lm3s6965_bsp`, old syscalls, UART, trace, OLED, or FreeRTOS.
Old targets keep their old SysTick, heap and linker behavior. Do not replace
this with the shared BSP object library: it carries incompatible syscall and
linker ownership. The target-local `_sbrk` overrides nosys for this executable.
No runtime stdio is used; lwIP diagnostics are suppressed and assertions trap.

## lwIP architecture port

`arch/cc.h` belongs to this example, not the reusable library or test tooling.
It selects `BYTE_ORDER LITTLE_ENDIAN` for Cortex-M3/GCC and the deterministic little-endian native test host.
`LWIP_DECLARE_MEMORY_ALIGNED` uses GCC's aligned attribute with `MEM_ALIGNMENT`.
The firmware alignment is four bytes. Native shared shims use eight bytes for 64-bit pointers.
`LWIP_PLATFORM_DIAG` suppresses diagnostics. `LWIP_PLATFORM_ASSERT` traps with `__builtin_trap()`.
Neither macro adds stdio, allocation, UART/ITM waits, or ISR protocol work.
Native builds disable lwIP byteorder aliases to avoid Darwin header collisions.
That build setting does not change the firmware byte order.

`arch/sys_arch.h` documents the NO_SYS port. It provides no semaphore, mailbox, thread, or interrupt-protection services.
`sys_now()` comes from `net_clock.c`. All lwIP calls remain serialized in foreground.
A reusable consumer supplies its own port configuration and exports it through its lwIP target.
The factory does not import this example's architecture headers. Keep these files with the example.

## Transport worker contract

`ftp_tcp.c` implements the three foreground-only functions in `ftp_tcp.h`:

- `err_t ftp_tcp_start(const ip4_addr_t *)`: start listeners bound to the supplied
  board address. Main calls it only when MAC ready and retries on failure on
  subsequent loops. Failure must leave no partial listeners. Success must be
  idempotent within the readiness epoch.
- `void ftp_tcp_service(uint32_t)`: bounded, nonblocking service every loop,
  including while down. Time is modular unsigned milliseconds. No printf,
  delays, socket/netconn calls or protocol work from interrupts.
- `void ftp_tcp_link_down(void)`: synchronous teardown after administrative and
  link down and ARP flush, before processing further RX. Abort and release all
  control/data/listening PCBs, retained pbufs and per-connection state; do not
  submit network traffic. Safe before successful start. A new ready epoch can
  start cleanly. TCP abort callback rules and pbuf ownership remain transport
  adapter responsibilities; do not rely on lwIP netif down to close TCP PCBs.

Main starts listeners after the ready transition, calls `sys_check_timeouts()`
and FTP service **every** iteration. The netif service performs at most one PHY
poll per 10 ms and consumes at most eight RX results per iteration, including
invalid/dropped frames. A timeout or `mac_ready == false` means not ready even if
PHY link and autonegotiation flags are true. Link loss sets both flags down,
flushes ARP, and calls teardown once. Down RX is consumed/discarded without pbuf
allocation. Runtime link recovery does not reinitialize the driver.

Address `192.168.7.2/24`, gateway zero, no DNS; local test MAC
`02:00:00:69:65:02`. Single netif, one foreground owner. Separate 1514-byte RX/TX
scratch arrays are necessary because RX processing can trigger TX. Driver reads
copy into RX scratch; `pbuf_take` supports chained buffers. Input ERR_OK transfers
ownership; input error frees the packet. Allocation failure drops the consumed
frame. TX copies the chain with `pbuf_copy_partial`; 14..1514 bytes only. The
caller retains all TX pbufs on every return. Driver BUSY maps to ERR_MEM; other
driver failures map ERR_IF. Successful submission is not delivery evidence.
Firmware's 1536-byte pool payload normally makes one RX pbuf per frame, but the
adapter still handles chains. Checksums are software-generated/validated.

## Timer contract and limits

`net_clock.c` owns a 4-byte-aligned volatile uint32 counter. Only SysTick writes
it after initialization. The ISR increments time only; aligned Cortex-M3 word
loads avoid tearing. Volatile preserves asynchronous compiler visibility, not
general C synchronization or compound-operation atomicity. Initialize once
before networking; no concurrent clock resets or CPU clock changes.

PLL configuration selects 50 MHz on success, 8 MHz main crystal on bounded PLL
failure. Main verifies RCC source/bypass/divider readback and rejects RCC2
USERCC2 override before enabling networking. This verifies configuration, not
physical frequency. CPU-clock SysTick CTRL=7 (ENABLE, TICKINT, CLKSOURCE),
CURRENT=0, RELOAD=49999 or 7999 respectively. Initialization rejects zero,
nonintegral-kHz and out-of-field periods. No COUNTFLAG polling. `sys_now()` reads
the accumulated ISR counter once. Native tests explicitly run ticks without
foreground sampling and force uint32 wrap.

`net_clock_elapsed` uses `(uint32_t)(now - since) >= interval`, with interval and
observation gaps both below 2^31 ms. Ambiguous long intervals are rejected.
PHY reschedules from the latest sample; no catch-up polling burst. Transport
must obey the same bound for its own elapsed comparisons. lwIP 2.2.1 timeouts use
`sys_now`, signed modular deadline ordering, and foreground timeout checking;
TCP has 250 ms timer cadence and ARP has 1 s cadence. Four timeout objects cover
this fixed configuration; no custom FTP lwIP timers are provisioned. FTP uses its own foreground timestamps. Adding lwIP timers requires revisiting the timeout budget.

Foreground service is bounded by eight driver reads, one bounded MII poll when
due, protocol input and service callbacks, not by measured wall time. Driver
MII waits are finite iteration counts, not latency guarantees. Interrupt
masking, disabled interrupts, pending tick coalescing and debug halt can lose
elapsed time. Source and host tests do not measure ISR latency, oscillator rate,
or worst-case timeout gaps on hardware.

## Memory accounting (final ELF, not runtime high-water acceptance)

`lwipopts.h` fixes: lwIP heap 12 KiB; pbuf pool 8 x 1536; separate pbuf metadata
pool 8; TCP PCB 8, listen PCB 2, segments 24, timeouts 4. TCP MSS 536,
window/send buffer 2144, send queue 16, out-of-order queue off, oversize zero.
ARP entries 4, queueing off. No UDP, DHCP, DNS, IPv6, IP fragmentation/reassembly,
raw-IP, sockets, netconn, ALTCP, FreeRTOS or libc allocation for lwIP.

The independently inspected final Arm Release ELF reports these static storage symbols (bytes):

| Symbol | Bytes |
| --- | ---: |
| `ram_heap` (including allocator sentinels) | 12304 |
| `memp_memory_PBUF_POOL_base` (payload + pbuf metadata) | 12416 |
| `memp_memory_TCP_PCB_base` | 1216 |
| `memp_memory_TCP_PCB_LISTEN_base` | 56 |
| `memp_memory_TCP_SEG_base` | 384 |
| `memp_memory_SYS_TIMEOUT_base` | 64 |
| `memp_memory_PBUF_base` | 128 |
| RX + TX scratch | 3028 |

These are already BSS allocations, **not additive budgets on top of final BSS**.
The PBUF_POOL metadata is already in its pool; MEMP_PBUF is a separate reference
pbuf pool. TCP copied payloads allocate within `ram_heap`, not extra SRAM outside
that budget. Pool metadata, stats, ARP table, TCP/netif/driver state, counter,
FTP core/file/transport state and alignment are reconciled in the final map.
Reusable core state occupies 1464 bytes plus a separate 4-byte borrowed storage pointer.
`main.c` owns the 40-byte RAM object and both 4096-byte backing arrays.
The final map places staging at `0x20000008`, committed storage at `0x20001008`,
and the RAM object at `0x20002008`. Buffer roles swap on commit.
Static SRAM is 39984 bytes: 8 bytes `.data` plus 39976 bytes actual `.bss`.
GNU size text is 27604 bytes; total flash load is 27612 bytes.
Static SRAM plus 2048-byte heap and 8192-byte stack reservations totals 50224 bytes.
The unassigned heap-to-stack gap is 15312 bytes.
Thus `39984 + 2048 + 8192 + 15312 = 65536`, without adding the pools again.
GNU `size` BSS includes both NOLOAD reservations and is not actual static BSS. `lwip_stats.mem` and `lwip_stats.memp[]` track used/max/error values;
protocol/link counters are also enabled without stats printing.

`lm3s6965_ftp.ld` reserves 2048 bytes in `.c_heap` immediately after static RAM,
and 8192 bytes in `.stack` ending at `0x20010000`; linker asserts heap end does
not cross stack limit `0x2000e000`. `_sbrk` accepts growth only inside the fixed
heap bounds and safe shrinking; errors preserve the break even for extreme
signed increments. This is an address separation contract, **not** proof the
runtime stack cannot overrun its reserve. No stack high-water measurement yet.

## Offline validation and pending verification

Historical platform-only commands ran successfully before tooling cleanup.
The old wrapper below is preserved as historical evidence, not a current command.
Its current equivalent is `./scripts/ethernet/test.sh platform`.

```sh
cmake -S . -B build/ftp-platform -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DLM3S6965_BUILD_ETHERNET_FTP=ON
cmake --build build/ftp-platform --target lm3s6965_ftp_platform lm3s6965_ftp_lwip -j4
scripts/ftp-platform-test.sh
cmake -S . -B build/ftp-off -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build/ftp-off -j4
arm-none-eabi-objdump -d build/ftp-platform/examples/ethernet-ftp/CMakeFiles/lm3s6965_ftp_platform.dir/net_clock.c.obj
arm-none-eabi-readelf -SW build/ftp-platform/examples/ethernet-ftp/CMakeFiles/lm3s6965_ftp_platform.dir/net_clock.c.obj
git diff --check
git -C third_party/lwip status --short
```

All four old firmware targets built with default OFF. ARM compilation uses
`-Wall -Wextra -Werror`. Disassembly confirms `sys_now` has one word load of the
counter, ISR is load/increment/store, BSS counter alignment is four.
Native ASan+UBSan tests use the real lwIP and deterministic driver/MMIO shims;
cover rate/fallback/rejection, ISR accumulation/wrap, 10 ms PHY cadence through
wrap, mac_ready gating, bounded/down RX, forced chain RX/TX, input/allocation/TX
failure ownership, stable ARP cleanup and link recovery/timeout teardown. A
separate native test verifies actual `_sbrk` growth/shrink/overflow behavior.
Native-only shim changes allocator alignment from four to eight for 64-bit
pointers, and disables lwIP's POSIX byteorder aliases to avoid Darwin header
collisions. Initial native checks exposed those ABI mismatches; corrected shim
runs are clean with UBSan halt-on-error, not suppressed sanitizer diagnostics.
Submodule status remains clean. Shared BSP/core/docs/submodules were not edited
by this platform implementation.

The commands above preserve the earlier platform-only validation boundary.
Latest independent tooling verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
The consumer compiled and linked only. Cleanup added no new firmware or hardware evidence.
See the [current suite guide](../../scripts/ethernet/README.md).
Run `./scripts/ethernet/test.sh ftp file platform` for FTP-related coverage.
Final independent verification subsequently built fresh FTP ON and all four OFF targets.
The final extraction OFF snapshot excluded both lwIP and the FTP example, and built the core/RAM archives.
All five FTP suites passed with ASan/UBSan (six executables).
A caller-owned src/include-only consumer compiled and linked with a different lwIP configuration.
It was not executed; no alternate-config runtime result is claimed.
The real lwIP packet suite covers simultaneous control/data paths, FIN allocation starvation,
QUIT refused input, output pressure, failures, retries, and quiescent heap/pool recovery.
See [transport evidence](TRANSPORT_VALIDATION.md) and [real lwIP tests](../../scripts/ethernet/ftp/ftp-lwip-validation.md).

The final extraction vector word at 0x3C is 0x1f1, resolving to strong SysTick_Handler at 0x1f0.
`sys_now` has one aligned word read. FreeRTOS retains its own handler and 32 KiB heap.
FTP retains no RTOS heap, console, OLED, or libc allocator implementation.
Heap bounds are `0x20009c30..0x2000a430`. Stack bounds are `0x2000e000..0x20010000`.
These are ELF/link-fit facts, not measured stack safety or runtime peak usage.

The adapter relies on pinned private `TF_CLOSEPEND` and `tcp_process_refused_data` contracts.
Upgrading lwIP requires review of FIN acceptance, QUIT draining, and ownership tests.
See [core API](FTP_CORE_API.md) for the exact contracts.
Runtime stack high-water, peak heap/pools, service gaps, physical clock frequency,
cable-cycle recovery, and hardware FTP acceptance remain pending authorized measurement.
No physical operation is claimed by this offline evidence or this documentation update.
