/* Link-only consoles for the production syscalls stdio dependency.
 * Never executed; neither a hardware console nor an allocator substitute. */
#include "uart.h"
#include "trace.h"
void uart0_write_byte(uint8_t byte) { (void)byte; }
void trace_write_byte(uint8_t byte) { (void)byte; }
