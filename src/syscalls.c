#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "trace.h"
#include "uart.h"

extern unsigned char _heap_start;
extern unsigned char _heap_static_end;
extern unsigned char _heap_end;

static uintptr_t g_heap_break;
static int g_heap_initialized;

void *_sbrk(ptrdiff_t increment)
{
    const uintptr_t start = (uintptr_t)&_heap_start;
    const uintptr_t base = (uintptr_t)&_heap_static_end;
    const uintptr_t end = (uintptr_t)&_heap_end;

    /* Static .heap inputs own [start, base); only [base, end) is dynamic.
     * Compare integer addresses, never subtract unrelated linker pointers. */
    if (start > base || base > end)
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    const uintptr_t previous = g_heap_initialized ? g_heap_break : base;
    if (previous < base || previous > end)
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    uintptr_t next;
    if (increment >= 0)
    {
        if ((uintmax_t)increment > (uintmax_t)(end - previous))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        next = previous + (uintptr_t)increment;
    }
    else
    {
        /* PTRDIFF_MIN cannot be negated in its signed type. */
        const uintmax_t amount = (uintmax_t)(-(increment + 1)) + 1u;
        if (amount > (uintmax_t)(previous - base))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        next = previous - (uintptr_t)amount;
    }
    g_heap_break = next;
    g_heap_initialized = 1;
    return (void *)previous;
}

int _write(int file_descriptor, char *buffer, int length)
{
    (void)file_descriptor;

    /* Both consoles receive every byte. The ITM write is a no-op until the trace
     * unit is enabled, so this path is safe whether or not SWO is being read. */
    for (int index = 0; index < length; index++)
    {
        const uint8_t byte = (uint8_t)buffer[index];

        uart0_write_byte(byte);
        trace_write_byte(byte);
    }

    return length;
}

int _read(int file_descriptor, char *buffer, int length)
{
    (void)file_descriptor;
    (void)buffer;
    (void)length;

    return 0;
}

int _close(int file_descriptor)
{
    (void)file_descriptor;

    return -1;
}

int _fstat(int file_descriptor, struct stat *status)
{
    (void)file_descriptor;

    status->st_mode = S_IFCHR;

    return 0;
}

int _isatty(int file_descriptor)
{
    (void)file_descriptor;

    return 1;
}

off_t _lseek(int file_descriptor, off_t offset, int whence)
{
    (void)file_descriptor;
    (void)offset;
    (void)whence;

    return 0;
}

void _exit(int status)
{
    (void)status;

    for (;;)
    {
    }
}

int _kill(int process_id, int signal_number)
{
    (void)process_id;
    (void)signal_number;

    errno = EINVAL;

    return -1;
}

int _getpid(void)
{
    return 1;
}
