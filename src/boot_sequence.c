#include "lm3s6965/boot_sequence.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    boot_reset_register_adapter_t underlying;
    uint32_t early;
    unsigned first;
} checked_reset_t;

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

static int aligned(const void *p, size_t alignment)
{
    return p != NULL && (uintptr_t)p % alignment == 0u;
}

static int boot_code(uintptr_t p)
{
#if defined(__arm__) || defined(__thumb__)
    return (p & 1u) != 0u && p < 0x8000u;
#else
    return p != 0u; /* HOST cannot establish target placement. */
#endif
}

static int checked_read(void *context, uint32_t *flags)
{
    checked_reset_t *c = context;
    uint32_t fresh;
    unsigned first = c->first;
    c->first = 0u; /* An attempted first read is never replayed. */
    if (c->underlying.read(c->underlying.context, &fresh) != 0 ||
        (first != 0u && fresh != c->early)) return -1;
    *flags = fresh;
    return 0;
}

static int checked_write(void *context, uint32_t flags)
{
    checked_reset_t *c = context;
    return c->underlying.write(c->underlying.context, flags);
}

void boot_sequence_init(boot_sequence_t *sequence)
{
    if (sequence == NULL) return;
    atomic_flag_clear(&sequence->entered);
    sequence->consumed = 0u;
    boot_runtime_watchdog_init(&sequence->bridge);
}

/* Validate only known spans and context starting addresses. Opaque context
 * extents and transitive callback lifetime remain trusted caller obligations. */
static int arguments(boot_sequence_t *sequence,
    const boot_sequence_parameters_t *p, boot_launch_decision_t *out)
{
    if (!aligned(p, _Alignof(boot_sequence_parameters_t)) ||
        !aligned(out, _Alignof(boot_launch_decision_t))) return 0;
    if (!aligned(p->continuity, _Alignof(boot_startup_t)) ||
        !aligned((const void *)p->diagnostic, _Alignof(uint32_t)) ||
        !aligned(p->producer_io, _Alignof(boot_watchdog_io_t)) ||
        !aligned(p->registers, _Alignof(boot_reset_register_adapter_t)) ||
        !aligned(p->store, _Alignof(boot_store_adapter_t)) ||
        !aligned(p->images, _Alignof(boot_select_images_t)) ||
        p->actual_hz == 0u || p->actual_hz > UINT32_MAX / 5u) return 0;
    const void *objects[] = {sequence, p->continuity,
        (const void *)p->diagnostic, out};
    const size_t object_sizes[] = {sizeof *sequence, sizeof *p->continuity,
        BOOT_DIAG_BYTES, sizeof *out};
    const void *inputs[] = {p, p->producer_io, p->registers, p->store, p->images};
    const size_t input_sizes[] = {sizeof *p, sizeof *p->producer_io,
        sizeof *p->registers, sizeof *p->store, sizeof *p->images};
    for (unsigned i = 0u; i < 4u; ++i)
    {
        for (unsigned j = i + 1u; j < 4u; ++j)
            if (overlaps(objects[i], object_sizes[i], objects[j],
                         object_sizes[j])) return 0;
        for (unsigned j = 0u; j < 5u; ++j)
            if (overlaps(objects[i], object_sizes[i], inputs[j],
                         input_sizes[j])) return 0;
    }
    if (p->producer_io->context != (void *)p->continuity->context ||
        !boot_code((uintptr_t)p->producer_io->read_register) ||
        !boot_code((uintptr_t)p->producer_io->write_register) ||
        !boot_code((uintptr_t)p->gate_delay) ||
        !boot_code((uintptr_t)boot_startup_gptm_read) ||
        p->registers->read == NULL || p->registers->write == NULL ||
        p->store->read == NULL || p->store->erase_page == NULL ||
        p->store->program_word == NULL || p->reader == NULL) return 0;
    const void *contexts[] = {p->registers->context, p->store->context,
        p->reader_context};
    for (unsigned i = 0u; i < 3u; ++i)
    {
        if (contexts[i] == NULL) continue; /* Stateless callbacks are allowed. */
        for (unsigned j = 0u; j < 4u; ++j)
            if (overlaps(contexts[i], 1u, objects[j], object_sizes[j])) return 0;
        for (unsigned j = 0u; j < 5u; ++j)
            if (overlaps(contexts[i], 1u, inputs[j], input_sizes[j])) return 0;
    }
#if defined(__arm__) || defined(__thumb__)
    uint32_t mask, ipsr, control;
    __asm__ volatile("mrs %0, primask\n mrs %1, ipsr\n mrs %2, control"
        : "=r"(mask), "=r"(ipsr), "=r"(control));
    if ((uintptr_t)p->continuity != 0x2000dbc0u ||
        (uintptr_t)p->diagnostic != 0x2000dfc0u || mask != 1u || ipsr != 0u ||
        (control & 1u) != 0u) return 0;
#endif
    return 1;
}

boot_sequence_result_t boot_sequence_run(boot_sequence_t *sequence,
    const boot_sequence_parameters_t *parameters, boot_launch_decision_t *out)
{
    boot_sequence_result_t result = {0};
    boot_sequence_parameters_t p;
    boot_watchdog_io_t producer;
    boot_store_adapter_t store;
    boot_select_images_t images;
    checked_reset_t reset;
    result.status = BOOT_SEQUENCE_ARGUMENT;
    if (!aligned(sequence, _Alignof(boot_sequence_t))) return result;
    /* Even guard acquisition must not modify an aliased output on rejection. */
    if (out != NULL && overlaps(sequence, sizeof *sequence, out, sizeof *out))
        return result;
    if (atomic_flag_test_and_set(&sequence->entered))
    {
        result.status = BOOT_SEQUENCE_BUSY;
        return result;
    }
    if (sequence->consumed != 0u)
    {
        result.status = BOOT_SEQUENCE_CONSUMED;
        goto leave;
    }
    if (!arguments(sequence, parameters, out)) goto leave;
    p = *parameters;
    producer = *p.producer_io;
    store = *p.store;
    images = *p.images;
    reset.underlying = *p.registers;
    reset.first = 1u;
    sequence->consumed = 1u;
    result.status = BOOT_SEQUENCE_FAILED;
    result.stage = BOOT_SEQUENCE_EARLY_READ;
    if (reset.underlying.read(reset.underlying.context, &reset.early) != 0)
        goto leave; /* Continuity may still be stale; do not inspect it. */
    result.stage = BOOT_SEQUENCE_PRODUCER;
    if (!boot_startup_producer_begin_io(p.continuity, p.diagnostic,
            reset.early, p.actual_hz, &producer, p.gate_delay)) goto leave;
    const boot_reset_register_adapter_t checked = {
        .context = &reset, .read = checked_read, .write = checked_write
    };
    result.stage = BOOT_SEQUENCE_RUNTIME;
    result.runtime = boot_runtime_watchdog_run(&sequence->bridge,
        &p.continuity->watchdog, &checked, &store, &images,
        p.reader, p.reader_context, out);
    if (result.runtime.status == BOOT_RUNTIME_WATCHDOG_OK)
        result.status = BOOT_SEQUENCE_OK;
leave:
    atomic_flag_clear(&sequence->entered);
    return result;
}
