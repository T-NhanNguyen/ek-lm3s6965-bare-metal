/* Synthetic static root only; never executed. Real sources + official libc/libgcc. */
#include "lm3s6965/boot_startup_producer.h"
_Static_assert(sizeof(boot_startup_gptm_t)==40,"ARM40 context prefix");
_Static_assert(sizeof(((boot_startup_t *)0)->context)==64,"context64");
_Static_assert(sizeof(boot_startup_t)==1024,"capsule1024");
_Noreturn void Reset_Handler(void);
__attribute__((used,section(".isr_vector")))
const uintptr_t producer_vectors[59]={0x20010000,(uintptr_t)Reset_Handler};
_Noreturn void Reset_Handler(void) {
    __asm__ volatile("cpsid i" ::: "memory");
    boot_startup_t *c=&boot_startup_continuity;
    /* Volatile runtime Hz prevents arithmetic constant folding in closure. */
    uint32_t hz=*(volatile uint32_t *)0x400fe060u;
    if(boot_startup_producer_begin(c,(volatile uint32_t *)0x2000dfc0u,0,hz)) {
        boot_startup_sample_t sample;
        (void)boot_startup_gptm_read(c->context,&sample);
    }
    for(;;) __asm__ volatile("nop");
}
