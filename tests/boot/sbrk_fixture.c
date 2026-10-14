/* Compile the whole syscall unit with isolated names: no extraction or
 * production test controls. Non-allocator syscalls are never invoked. */
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "trace.h"
#include "uart.h"

void *allocator_sbrk(ptrdiff_t increment);
#ifdef TEST_GENERIC
#define _sbrk allocator_sbrk
#define _write fixture_write
#define _read fixture_read
#define _close fixture_close
#define _fstat fixture_fstat
#define _isatty fixture_isatty
#define _lseek fixture_lseek
#define _exit fixture_exit
#define _kill fixture_kill
#define _getpid fixture_getpid
int fixture_write(int fd, char *buffer, int length);
int fixture_read(int fd, char *buffer, int length);
int fixture_close(int fd);
int fixture_fstat(int fd, struct stat *status);
int fixture_isatty(int fd);
off_t fixture_lseek(int fd, off_t offset, int whence);
void fixture_exit(int status);
int fixture_kill(int pid, int signal_number);
int fixture_getpid(void);
#include "../../src/syscalls.c"
void uart0_write_byte(uint8_t byte) { (void)byte; }
void trace_write_byte(uint8_t byte) { (void)byte; }
#else
#define _sbrk allocator_sbrk
#include "../../examples/ethernet-ftp/ftp_sbrk.c"
#endif
