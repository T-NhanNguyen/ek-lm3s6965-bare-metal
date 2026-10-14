/* Isolated QEMU fixture: no production console, diagnostic ABI or allocator changes. */
#include "lm3s6965/boot_verify.h"
#include "../verify_fixtures.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char _heap_static_end, _heap_end;
void *__real_calloc(size_t, size_t);
void __real_free(void *);
void *__real__sbrk(ptrdiff_t);
static struct { void *pointer; size_t bytes; } slots[128];
static uint32_t live, peak, count, count_peak, calls, failed, sbrk_failed;
static uint32_t backing_peak, failed_increment, tracking_error, inject;
static char command[32], report[512];
static uint32_t used;
static uintptr_t semihost(uint32_t op, const void *arg)
{
    register uintptr_t r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026u, status};
    semihost(4u, report);
    semihost(0x20u, args);
    for (;;) {}
}
void *__wrap_calloc(size_t n, size_t size)
{
    ++calls;
    if (inject && calls == 1u) { ++failed; return NULL; }
    void *p = __real_calloc(n, size);
    if (!p) { ++failed; return NULL; }
    if (size && n > SIZE_MAX / size) { tracking_error = 1; return p; }
    for (unsigned i = 0; i < 128; ++i)
        if (!slots[i].pointer) {
            slots[i].pointer = p; slots[i].bytes = n * size;
            live += n * size; ++count;
            if (live > peak) peak = live;
            if (count > count_peak) count_peak = count;
            return p;
        }
    tracking_error = 1;
    return p;
}
void __wrap_free(void *p)
{
    if (p) {
        unsigned i;
        for (i = 0; i < 128; ++i) if (slots[i].pointer == p) break;
        if (i == 128) tracking_error = 1;
        else { live -= slots[i].bytes; --count; slots[i].pointer = NULL; }
    }
    __real_free(p);
}
void *__wrap__sbrk(ptrdiff_t increment)
{
    void *p = __real__sbrk(increment);
    if (p == (void *)-1) {
        ++sbrk_failed;
        if (increment > 0 && (uint32_t)increment > failed_increment)
            failed_increment = (uint32_t)increment;
    } else {
        uintptr_t next = (uintptr_t)p + increment;
        uint32_t backing = next - (uintptr_t)&_heap_static_end;
        if (backing > backing_peak) backing_peak = backing;
    }
    return p;
}
/* No prologue, stores or calls: the scanner cannot deepen the painted stack. */
__attribute__((naked, noinline)) static uint32_t stack_depth(void)
{
    __asm__("ldr r0, =0x2000e000\nldr r1, =0x20010000\nldr r2, =0xa5c39e71\n"
            "1: cmp r0, r1\nbhs 2f\nldr r3, [r0]\ncmp r3, r2\nbne 2f\n"
            "adds r0, #4\nb 1b\n2: subs r0, r1, r0\nbx lr\n");
}
__attribute__((noinline)) static void burn(void)
{
    volatile uint32_t words[128];
    for (unsigned i = 0; i < 128; ++i) words[i] = i;
    __asm__ volatile("" : : "r"(words) : "memory");
}
static void field(const char *name, uint32_t value)
{
    while (*name) report[used++] = *name++;
    report[used++] = '=';
    char digits[10]; unsigned n = 0;
    do { digits[n++] = '0' + value % 10u; value /= 10u; } while (value);
    while (n) report[used++] = digits[--n];
    report[used++] = ' ';
}
typedef struct { uint32_t slot, mode, length, kind, completed; } stream_t;
static size_t read_payload(void *ctx, uint32_t address, uint8_t *out, size_t requested)
{
    stream_t *s = ctx;
    uint32_t base = s->slot ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE;
    uint32_t offset = address - base;
    if (address < base || offset != s->completed || offset > s->length ||
        requested > s->length - offset || requested > BOOT_VERIFY_CHUNK_BYTES) {
        tracking_error = 1; return 0;
    }
    if (s->kind == 8) return 0;
    for (size_t i = 0; i < requested; ++i) {
        uint32_t pos = offset + i;
        uint8_t b = (uint8_t)(pos * 17u + s->slot * 31u);
        uint32_t msp = s->mode == 2 ? 0x2000fffcu : BOOT_INITIAL_MSP;
        if ((pos >= 100 && pos < 108) || pos >= s->length - 4) b = 0xff;
        if (pos < 4) b = msp >> (8 * pos);
        else if (pos < 8) b = (base + 9u) >> (8 * (pos - 4));
        if (s->kind == 6 && pos == 20) b ^= 1;
        out[i] = b;
    }
    s->completed += requested;
    return requested;
}
static int progress(void *ctx, boot_verify_phase_t phase, uint32_t done, uint32_t total)
{
    stream_t *s = ctx; (void)done; (void)total;
    return !(s->kind == 9 && phase == BOOT_VERIFY_MANIFEST_AUTHENTICATED);
}
int main(void)
{
    uint32_t args[2] = {(uint32_t)(uintptr_t)command, sizeof command};
    if (semihost(0x15, args)) return 1; /* Missing reporting => runner rejects. */
    unsigned test = 0;
    for (unsigned i = 0; i < sizeof command && command[i] >= '0' && command[i] <= '9'; ++i) {
        test = test * 10u + command[i] - '0';
        if (test > 26) { strcpy(report, "INVALID CASE\n"); finish(1); }
    }
    volatile uint32_t *diag = (volatile uint32_t *)0x2000dfc0;
    for (unsigned i = 0; i < 16; ++i) diag[i] = 0x735ac901u;
    uint32_t before = stack_depth(), result = 0, expected = 0, output_ok = 1;
    if (stack_depth() != before) tracking_error = 1;
    if (test <= 2) {
        burn();
        if (test == 2) {
            void *a = __wrap_calloc(1, 37), *b = __wrap_calloc(1, 73);
            if (!a || !b || peak != 110 || count_peak != 2) tracking_error = 1;
            __wrap_free(a); __wrap_free(b);
        }
    } else {
        unsigned kind = test - 3;
        stream_t s = {0, 0, BOOT_PAYLOAD_MIN, kind, 0};
        if (test >= 21) {
            static const unsigned kinds[] = {6, 8, 9};
            kind = kinds[(test - 21) % 3];
            s.slot = (test - 21) / 3; s.mode = 1;
        } else if (test >= 14) { kind = test - 10; s.slot = 1; }
        else if (kind < 4) { s.slot = kind / 2; s.mode = kind % 2; }
        s.kind = kind;
        if (kind == 7) s.mode = 2;
        s.length = s.mode == 1 ? BOOT_PAYLOAD_MAX : BOOT_PAYLOAD_MIN;
        uint8_t wire[BOOT_MANIFEST_BYTES];
        memcpy(wire, fixture_manifest[s.slot * 7 + s.mode], sizeof wire);
        if (kind == 4) wire[0] ^= 1;
        if (kind == 5) wire[100] ^= 1;
        inject = kind == 10;
        static const uint32_t errors[] = {0,0,0,0,2,4,8,9,6,7,5};
        expected = kind <= 10 ? errors[kind] : 99;
#ifdef RESOURCE_OLD_FTP_2K
        /* Valid structural admission must reach genuine crypto OOM. */
        if (kind != 4) expected = BOOT_VERIFY_ERROR_CRYPTO;
#endif
        boot_verified_image_t image, saved;
        memset(&image, 0x5a, sizeof image); memcpy(&saved, &image, sizeof image);
        result = boot_verify_stored(wire, sizeof wire, s.slot, read_payload, &s,
                                    progress, &s, &image);
        if (result) output_ok = memcmp(&image, &saved, sizeof image) == 0;
        else output_ok = image.manifest.slot == s.slot &&
            image.manifest.payload_length == s.length && image.initial_msp == BOOT_INITIAL_MSP &&
            image.reset_vector == (s.slot ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE) + 9u &&
            s.completed == s.length && !memcmp(image.manifest.payload_hash, wire + 48, 32);
    }
    uint32_t depth = stack_depth(), guards = 1;
    if (stack_depth() != depth) tracking_error = 1;
    for (unsigned i = 0; i < 16; ++i) if (diag[i] != 0x735ac901u) guards = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (((volatile uint32_t *)0x2000e000)[i] != 0xa5c39e71) guards = 0;
    uint32_t msp; __asm__ volatile("mrs %0, msp" : "=r"(msp));
    uint32_t status = test == 1 || result != expected || !output_ok || live || count ||
        tracking_error || !guards || msp < 0x2000e010 || msp > 0x20010000 ||
        (test <= 2 && depth < before + 512u);
    strcpy(report, "RESOURCE "); used = 9;
    field("test", test); field("result", result); field("expected", expected);
    field("peak", peak); field("live", live); field("count_peak", count_peak);
    field("count", count); field("calls", calls); field("failed", failed);
    field("backing", backing_peak); field("break", (uintptr_t)__real__sbrk(0) - (uintptr_t)&_heap_static_end);
    field("sbrk_failed", sbrk_failed); field("increment", failed_increment);
    field("capacity", (uintptr_t)&_heap_end - (uintptr_t)&_heap_static_end);
    field("stack", depth); field("before", before); field("guards", guards);
    field("msp", msp); field("output", output_ok); field("tracking", tracking_error);
    field("status", status);
    report[used++] = '\n'; report[used] = 0;
    finish(status);
    return 0;
}
