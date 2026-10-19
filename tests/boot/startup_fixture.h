#ifndef STARTUP_FIXTURE_H
#define STARTUP_FIXTURE_H
#include "lm3s6965/boot_startup.h"
#if defined(FOREGROUND_CONTINUITY_TEST)
#include "lm3s6965/boot_startup_gptm.h"
#define STARTUP_HZ 50000000u
#define STARTUP_LOAD 250000000u
#else
#define STARTUP_HZ 100u
#define STARTUP_LOAD 500u
#endif
/* GCC native byte-storage view, without strict-aliasing assumptions. */
typedef struct __attribute__((may_alias)) {
    uint64_t us;
    uint32_t load, ctl, lock, feeds, reads, writes, rcc, rcc2, digest;
    uint32_t clock_reads, fail_clock_on_read, dispatch_writes, dispatch_feeds;
} startup_model_t;
#if defined(FOREGROUND_CONTINUITY_TEST)
/* A separate model, not an overlapping GPTM descriptor. No boot BSS. */
typedef struct __attribute__((may_alias)) {
    startup_model_t startup;
    uint32_t gate, cfg, tamr, timer_ctl, imr, tailr, ris;
    uint32_t timer, tar_reads, timer_reads, timer_writes;
    uint32_t final_us, saw_start;
} foreground_model_t;
_Static_assert(sizeof(foreground_model_t) <= 512, "retained reserve budget");
static inline __attribute__((always_inline)) startup_model_t *startup_model(boot_startup_t *c)
{ return (startup_model_t *)(void *)c->reserve; }
static inline __attribute__((always_inline)) foreground_model_t *foreground_model(boot_startup_t *c)
{ return (foreground_model_t *)(void *)c->reserve; }
static inline __attribute__((always_inline)) boot_startup_t *startup_capsule(void *context)
{ return (boot_startup_t *)((uint8_t *)context - offsetof(boot_startup_t, context)); }
#else
_Static_assert(sizeof(startup_model_t) <= 64, "retained context budget");
static inline __attribute__((always_inline)) startup_model_t *startup_model(boot_startup_t *c)
{ return (startup_model_t *)(void *)c->context; }
#endif
static inline uint32_t diagnostic_digest(volatile uint32_t *p)
{
    uint32_t hash = 0;
    for (unsigned i = 0; i < 16; ++i) hash = (hash * 33u) ^ p[i];
    return hash;
}
static inline void semi(unsigned operation, const void *argument)
{
    register unsigned r0 __asm__("r0") = operation;
    register const void *r1 __asm__("r1") = argument;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}
static inline _Noreturn void finish(unsigned status)
{
    const uint32_t block[2] = {0x20026, status};
    semi(0x20, block);
    for (;;) {}
}
#define CHECK(x) do { if (!(x)) { semi(4, "STARTUP FAIL " #x "\n"); finish(1); } } while (0)
#endif
