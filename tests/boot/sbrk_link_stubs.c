/* Bare-metal link fixture only; no libc or hardware is executed. */
#include <errno.h>
int *__errno(void)
{
    static int error;
    return &error;
}
