#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lm3s6965/trace.h"
#include "lm3s6965/uart.h"

extern uint32_t _end;
extern uint32_t _estack;

#define HEAP_STACK_RESERVE_BYTES 1024u

static uintptr_t g_heap_break = 0u;

void *_sbrk(ptrdiff_t increment)
{
    if (g_heap_break == 0u)
    {
        g_heap_break = (uintptr_t)&_end;
    }

    const uintptr_t stack_limit = (uintptr_t)&_estack - HEAP_STACK_RESERVE_BYTES;

    if ((g_heap_break + (uintptr_t)increment) > stack_limit)
    {
        errno = ENOMEM;
        return (void *)-1;
    }

    const uintptr_t previous_break = g_heap_break;
    g_heap_break += (uintptr_t)increment;

    return (void *)previous_break;
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
