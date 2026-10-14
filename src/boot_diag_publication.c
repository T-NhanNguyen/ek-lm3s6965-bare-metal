#include "lm3s6965/boot_diag_publication.h"

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}
static bool argument(boot_diag_publication_t *p, const void *a, size_t n)
{
    if (p == NULL || p->words == NULL || ((uintptr_t)p->words & 3u) != 0u)
        return false;
#if defined(__arm__) || defined(__thumb__)
    if (p->word != NULL || p->barrier != NULL) return false;
#endif
    if (overlaps(p, sizeof(*p), (const void *)p->words, BOOT_DIAG_BYTES)) return false;
    return a == NULL || (!overlaps(a, n, p, sizeof(*p)) &&
        !overlaps(a, n, (const void *)p->words, BOOT_DIAG_BYTES));
}
static uint32_t enter(void)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t mask;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(mask) :: "memory");
    return mask;
#else
    return 0;
#endif
}
static void leave(uint32_t mask)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("msr primask, %0" :: "r"(mask) : "memory");
#else
    (void)mask;
#endif
}
static bool foreground(void)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr) :: "memory");
    return ipsr == 0;
#else
    return true;
#endif
}
static void barrier(boot_diag_publication_t *p)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dmb" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
    if (p->barrier != NULL) p->barrier(p->user);
}
/* Kept out of line so the target store width has a direct inspection gate. */
__attribute__((noinline)) static void native_store(volatile uint32_t *words, unsigned i, uint32_t v)
{
    words[i] = v;
}
static bool word(boot_diag_publication_t *p, unsigned i, bool write, uint32_t *v)
{
    if (p->word != NULL) return p->word(p->user, i, write, v);
    if (write) native_store(p->words, i, *v);
    else *v = p->words[i];
    return true;
}
static boot_diag_publication_result_t lock(boot_diag_publication_t *p, uint32_t *mask)
{
    if (!foreground()) return BOOT_DIAG_PUBLICATION_ISR;
    *mask = enter();
    if (p->busy) { leave(*mask); return BOOT_DIAG_PUBLICATION_BUSY; }
    p->busy = true;
    return BOOT_DIAG_PUBLICATION_OK;
}
static void unlock(boot_diag_publication_t *p, uint32_t mask)
{
    p->busy = false;
    leave(mask);
}
static boot_diag_publication_result_t read_record(boot_diag_publication_t *p, boot_diag_t *r)
{
    uint8_t wire[BOOT_DIAG_BYTES];
    unsigned i;
    for (i = 0; i < 16; ++i) {
        uint32_t v = 0;
        if (!word(p, i, false, &v)) return BOOT_DIAG_PUBLICATION_READ;
        wire[i*4] = (uint8_t)v; wire[i*4+1] = (uint8_t)(v >> 8);
        wire[i*4+2] = (uint8_t)(v >> 16); wire[i*4+3] = (uint8_t)(v >> 24);
    }
    return boot_diag_decode(wire, sizeof(wire), BOOT_DIAG_PUBLISHED, r) == BOOT_DIAG_OK ?
        BOOT_DIAG_PUBLICATION_OK : BOOT_DIAG_PUBLICATION_INVALID;
}
static boot_diag_publication_result_t publish(boot_diag_publication_t *p,
    const boot_diag_t *r, const boot_diag_context_t *c)
{
    uint8_t wire[BOOT_DIAG_BYTES];
    uint32_t candidate[16], v = 0;
    unsigned i;
    /* Complete codec validation and staging before even validity is touched. */
    if (boot_diag_encode(r, BOOT_DIAG_PUBLISHED, wire, sizeof(wire)) != BOOT_DIAG_OK)
        return BOOT_DIAG_PUBLICATION_INVALID;
    for (i = 0; i < 16; ++i) candidate[i] = (uint32_t)wire[i*4] |
        ((uint32_t)wire[i*4+1] << 8) | ((uint32_t)wire[i*4+2] << 16) |
        ((uint32_t)wire[i*4+3] << 24);
    if (!word(p, 3, true, &v)) {
        p->associated = false;
        return BOOT_DIAG_PUBLICATION_UNCERTAIN;
    }
    p->associated = false;
    barrier(p);
    for (i = 0; i < 16; ++i) if (i != 3) {
        if (!word(p, i, true, &candidate[i])) return BOOT_DIAG_PUBLICATION_UNCERTAIN;
    }
    p->context = *c;
    barrier(p);
    v = 1;
    if (!word(p, 3, true, &v)) return BOOT_DIAG_PUBLICATION_UNCERTAIN;
    p->associated = true;
    return BOOT_DIAG_PUBLICATION_OK;
}
boot_diag_publication_result_t boot_diag_publication_boot(boot_diag_publication_t *p, uint32_t flags)
{
    boot_diag_t r; boot_diag_context_t c; uint32_t mask;
    boot_diag_publication_result_t result;
    if (!argument(p, NULL, 0)) return BOOT_DIAG_PUBLICATION_ARGUMENT;
    (void)boot_diag_init(flags, &r, &c);
    result = lock(p, &mask);
    if (result != BOOT_DIAG_PUBLICATION_OK) return result;
    result = publish(p, &r, &c); unlock(p, mask); return result;
}
boot_diag_publication_result_t boot_diag_publication_adopt(boot_diag_publication_t *p,
    const boot_diag_context_t *c, bool trusted)
{
    boot_diag_t r; uint32_t mask; boot_diag_publication_result_t result;
    if (c == NULL || !argument(p, c, sizeof(*c))) return BOOT_DIAG_PUBLICATION_ARGUMENT;
    if (!trusted) return BOOT_DIAG_PUBLICATION_UNASSOCIATED;
    result = lock(p, &mask); if (result != BOOT_DIAG_PUBLICATION_OK) return result;
    result = read_record(p, &r);
    if (result == BOOT_DIAG_PUBLICATION_OK) { p->context = *c; p->associated = true; }
    unlock(p, mask); return result;
}
boot_diag_publication_result_t boot_diag_publication_progress(boot_diag_publication_t *p,
    uint32_t stage, uint32_t health)
{
    boot_diag_t r; boot_diag_context_t c; uint32_t mask; boot_diag_publication_result_t result;
    if (!argument(p, NULL, 0)) return BOOT_DIAG_PUBLICATION_ARGUMENT;
    if (stage > 5 || (health & ~BOOT_DIAG_HEALTH_MASK) != 0) return BOOT_DIAG_PUBLICATION_INVALID;
    result = lock(p, &mask); if (result != BOOT_DIAG_PUBLICATION_OK) return result;
    if (!p->associated) result = BOOT_DIAG_PUBLICATION_UNASSOCIATED;
    else {
        result = read_record(p, &r);
        if (result == BOOT_DIAG_PUBLICATION_OK) {
            r.stage = stage; r.health = health; c = p->context;
            /* Existing availability remains unchanged, including unknowns. */
            result = publish(p, &r, &c);
        }
    }
    unlock(p, mask); return result;
}
boot_diag_publication_result_t boot_diag_publication_snapshot(boot_diag_publication_t *p,
    boot_diag_t *r, boot_diag_context_t *c)
{
    boot_diag_t copy; uint32_t mask; boot_diag_publication_result_t result;
    if (r == NULL || c == NULL || !argument(p, r, sizeof(*r)) ||
        !argument(p, c, sizeof(*c)) || overlaps(r, sizeof(*r), c, sizeof(*c)))
        return BOOT_DIAG_PUBLICATION_ARGUMENT;
    result = lock(p, &mask); if (result != BOOT_DIAG_PUBLICATION_OK) return result;
    result = p->associated ? read_record(p, &copy) : BOOT_DIAG_PUBLICATION_UNASSOCIATED;
    if (result == BOOT_DIAG_PUBLICATION_OK) { *r = copy; *c = p->context; }
    unlock(p, mask); return result;
}

boot_diag_publication_result_t boot_diag_publication_capture(boot_diag_publication_t *p,
    boot_diag_capture_fn capture, void *context)
{
    boot_diag_t r; boot_diag_context_t c; uint32_t mask;
    boot_diag_publication_result_t result;
    if (capture == NULL || !argument(p, NULL, 0)) return BOOT_DIAG_PUBLICATION_ARGUMENT;
    result = lock(p, &mask);
    if (result != BOOT_DIAG_PUBLICATION_OK) return result;
    result = p->associated ? read_record(p, &r) : BOOT_DIAG_PUBLICATION_UNASSOCIATED;
    if (result == BOOT_DIAG_PUBLICATION_OK) {
        c = p->context;
        if (!capture(context, &r, &c)) result = BOOT_DIAG_PUBLICATION_CAPTURE;
    }
    unlock(p, mask);
    return result;
}
