#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *allocator_sbrk(ptrdiff_t increment);
static unsigned checks;
static void check(int condition)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "allocator check %u failed\n", checks);
        exit(1);
    }
}
static void success(ptrdiff_t increment, uintptr_t old, uintptr_t next)
{
    errno = EDOM;
    check((uintptr_t)allocator_sbrk(increment) == old);
    check(errno == EDOM);
    check((uintptr_t)allocator_sbrk(0) == next);
    check(errno == EDOM);
}
static void failure(ptrdiff_t increment, uintptr_t current)
{
    errno = EDOM;
    check(allocator_sbrk(increment) == (void *)-1);
    check(errno == ENOMEM);
    success(0, current, current);
}
int main(void)
{
    /* The expected addresses are independent fixture constants, not results
     * computed from the allocator's linker symbols or its arithmetic. */
    const uintptr_t base = TEST_BASE;
    const uintptr_t end = TEST_END;
    if (TEST_START > TEST_BASE || TEST_BASE > TEST_END)
    {
        const ptrdiff_t invalid[] = {0, 1, -1, PTRDIFF_MIN, PTRDIFF_MAX};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        {
            errno = EDOM;
            check(allocator_sbrk(invalid[i]) == (void *)-1);
            check(errno == ENOMEM);
        }
    }
    else
    {
        const ptrdiff_t capacity = (ptrdiff_t)(end - base);
        success(0, base, base);
        failure(-1, base);
        failure(PTRDIFF_MIN, base);
        failure(PTRDIFF_MAX, base);
        failure(capacity + 1, base);
        success(capacity, base, end);
        failure(1, end);
        failure(PTRDIFF_MAX, end);
        failure(PTRDIFF_MIN, end);
        failure(-capacity - 1, end);
        success(-capacity, end, base);
        if (capacity > 1)
        {
            success(1, base, base + 1u);
            failure(-2, base + 1u);
            failure(capacity, base + 1u);
            success(capacity - 1, base + 1u, end);
            success(-1, end, end - 1u);
            success(1, end - 1u, end);
            success(-capacity, end, base);
        }
        failure(-1, base);
        /* end == diagnostic start is allowed as a break, but no byte there
         * can be allocated. The complete stack is farther above it. */
        if (end == 0x2000dfc0u)
        {
            success(capacity, base, end);
            failure(64, end);
            failure(8192 + 64, end);
            success(-capacity, end, base);
        }
    }
    printf("allocator: %u checks passed\n", checks);
    return 0;
}
