#include "lm3s6965/boot_handoff.h"
#include "boot_handoff_internal.h"
#include "boot_watchdog_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

boot_handoff_result_t boot_handoff_init(boot_handoff_t *h, const boot_handoff_io_t *io)
{
    if (h == NULL || io == NULL || io->operation == NULL || io->terminal_intent == NULL)
        return BOOT_HANDOFF_ARGUMENT;
    h->io = *io;
    h->entered = (atomic_flag)ATOMIC_FLAG_INIT;
    h->blocked = h->finished = 0u;
    return BOOT_HANDOFF_OK;
}

boot_handoff_result_t boot_handoff_admit(const boot_handoff_request_t *r,
                                        boot_handoff_intent_t *out)
{
    boot_handoff_intent_t intent = {0};
    uint32_t base, length, target;
    if (r == NULL || out == NULL) return BOOT_HANDOFF_ARGUMENT;
    if (r->selected_slot != BOOT_SLOT_A && r->selected_slot != BOOT_SLOT_B)
        return BOOT_HANDOFF_ARGUMENT;
    base = r->selected_slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    length = r->image.manifest.payload_length;
    target = r->image.reset_vector & ~UINT32_C(1);
    if (r->image.manifest.slot != r->selected_slot ||
        r->image.manifest.execution_base != base || base % BOOT_VECTOR_ALIGNMENT != 0u ||
        length < BOOT_PAYLOAD_MIN || length > BOOT_PAYLOAD_MAX || length % 4u != 0u ||
        r->image.initial_msp != BOOT_INITIAL_MSP ||
        (r->image.reset_vector & UINT32_C(1)) == 0u ||
        target < base || target - base >= length ||
        r->observed.ipsr != 0u || (r->observed.control & ~UINT32_C(2)) != 0u ||
        r->observed.primask > 1u || (r->observed.basepri & ~UINT32_C(0xe0)) != 0u ||
        r->observed.faultmask > 1u || r->watchdog_epoch == 0u ||
        r->watchdog_epoch == UINT32_MAX) return BOOT_HANDOFF_ARGUMENT;
    intent.vtor = base;
    intent.msp = r->image.initial_msp;
    intent.reset_vector = r->image.reset_vector;
    intent.application_epoch = r->watchdog_epoch + 1u;
    *out = intent;
    return BOOT_HANDOFF_OK;
}

static boot_handoff_result_t leave(boot_handoff_t *h, boot_handoff_result_t result)
{
    atomic_flag_clear_explicit(&h->entered, memory_order_release);
    return result;
}

static boot_handoff_result_t fault(boot_handoff_t *h)
{
    /* STE: an unsuccessful callback may already have acted. No continuation,
     * exclusion release or rollback is safe. Quarantine has no recovery API. */
    h->blocked = 1u;
    return BOOT_HANDOFF_IO;
}

static boot_handoff_result_t run(boot_handoff_t *h, boot_watchdog_t *w,
    const boot_handoff_request_t *request, boot_handoff_intent_t *out,
    unsigned held)
{
    static const boot_handoff_operation_t order[] = {
        BOOT_HANDOFF_EXCLUDE, BOOT_HANDOFF_STOP_SYSTICK,
        BOOT_HANDOFF_DISABLE_BANK0, BOOT_HANDOFF_DISABLE_BANK1,
        BOOT_HANDOFF_QUIESCE_SOURCES, BOOT_HANDOFF_CLEAR_CORE_PENDING,
        BOOT_HANDOFF_CLEAR_BANK0_PENDING, BOOT_HANDOFF_CLEAR_BANK1_PENDING,
        BOOT_HANDOFF_BARRIER_BEFORE_VTOR, BOOT_HANDOFF_SET_VTOR,
        BOOT_HANDOFF_BARRIER_AFTER_VTOR
    };
    boot_handoff_request_t snapshot;
    boot_handoff_intent_t intent;
    boot_handoff_result_t result;
    size_t i;
    if (h == NULL || w == NULL || request == NULL || out == NULL)
        return BOOT_HANDOFF_ARGUMENT;
    if (h->blocked != 0u) return BOOT_HANDOFF_BLOCKED;
    if (h->finished != 0u) return BOOT_HANDOFF_STATE;
    snapshot = *request;
    result = boot_handoff_admit(&snapshot, &intent);
    if (result != BOOT_HANDOFF_OK) return result;
    /* Standalone entry briefly checks idle ownership. Protected launch already
     * holds both guards. Neither path excludes external hardware access. */
    if (held == 0u &&
        atomic_flag_test_and_set_explicit(&w->entered, memory_order_acquire))
        return BOOT_HANDOFF_BUSY;
    result = w->configured == 0u || w->blocked != 0u ||
        w->owner != BOOT_WATCHDOG_BOOT || w->epoch != snapshot.watchdog_epoch ||
        w->epoch == UINT32_MAX ? BOOT_HANDOFF_STATE : BOOT_HANDOFF_OK;
    if (held == 0u) atomic_flag_clear_explicit(&w->entered, memory_order_release);
    if (result != BOOT_HANDOFF_OK) return result;
    for (i = 0u; i < sizeof order / sizeof order[0]; ++i)
    {
        uint32_t value = order[i] == BOOT_HANDOFF_SET_VTOR ? intent.vtor : 0u;
        if (h->io.operation(h->io.context, order[i], value) != 0) return fault(h);
    }
    /* One-way ownership is not rolled back even if terminal intent is ambiguous. */
    if ((held != 0u ? boot_watchdog_transfer_held(w, snapshot.watchdog_epoch) :
         boot_watchdog_transfer(w, snapshot.watchdog_epoch)) != BOOT_WATCHDOG_OK)
        return fault(h);
    if (h->io.terminal_intent(h->io.context, &intent) != 0) return fault(h);
    h->finished = 1u;
    *out = intent;
    return BOOT_HANDOFF_OK;
}

boot_handoff_result_t boot_handoff_run_held(boot_handoff_t *h, boot_watchdog_t *w,
    const boot_handoff_request_t *request, boot_handoff_intent_t *out)
{
    return run(h, w, request, out, 1u);
}

boot_handoff_result_t boot_handoff_run(boot_handoff_t *h, boot_watchdog_t *w,
    const boot_handoff_request_t *request, boot_handoff_intent_t *out)
{
    if (h == NULL || w == NULL || request == NULL || out == NULL)
        return BOOT_HANDOFF_ARGUMENT;
    if (atomic_flag_test_and_set_explicit(&h->entered, memory_order_acquire))
        return BOOT_HANDOFF_BUSY;
    return leave(h, run(h, w, request, out, 0u));
}
