#include "lm3s6965/boot_runtime_watchdog.h"
#include "boot_watchdog_internal.h"
#include "boot_handoff_internal.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_runtime.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct
{
    boot_watchdog_t *watchdog;
    boot_runtime_watchdog_result_t *result;
} progress_context_t;

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

static int progress(void *context, boot_verify_phase_t phase,
                    uint32_t completed, uint32_t total)
{
    progress_context_t *p = context;
    boot_watchdog_t *w = p->watchdog;
    boot_runtime_watchdog_result_t *r = p->result;
    r->valid |= BOOT_RUNTIME_WATCHDOG_CALLBACK;
    r->operation = BOOT_RUNTIME_WATCHDOG_REPORT;
    r->phase = phase;
    r->epoch = w->epoch;
    r->verification = w->verification;
    r->completed = completed;
    r->total = total;
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN)
    {
        if (w->verification == UINT32_MAX)
        {
            r->watchdog = BOOT_WATCHDOG_STATE;
            return 0;
        }
        ++r->verification;
    }
    r->watchdog = boot_watchdog_report_held(w, w->epoch, r->verification,
                                            phase, completed, total);
    if (r->watchdog != BOOT_WATCHDOG_OK) return 0;
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN) return 1;
    r->operation = BOOT_RUNTIME_WATCHDOG_SERVICE;
    r->watchdog = boot_watchdog_service_held(w, BOOT_WATCHDOG_BOOT, w->epoch);
    return r->watchdog == BOOT_WATCHDOG_OK;
}

void boot_runtime_watchdog_init(boot_runtime_watchdog_t *bridge)
{
    if (bridge == NULL) return;
    atomic_flag_clear(&bridge->entered);
    bridge->consumed = 0u;
    boot_runtime_init(&bridge->runtime);
}

static boot_runtime_watchdog_result_t run(
    boot_runtime_watchdog_t *bridge, boot_watchdog_t *w,
    boot_handoff_t *h, const boot_handoff_context_t *observed, unsigned launch,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    void *out)
{
    boot_runtime_watchdog_result_t result = {0};
    boot_reset_register_adapter_t reset_io;
    boot_store_adapter_t storage;
    boot_select_images_t prefixes;
    boot_runtime_watchdog_launch_t output = {0};
    boot_handoff_context_t observation = {0};
    boot_handoff_request_t request = {0};
    progress_context_t context = {w, &result};
    const void *inputs[4];
    size_t sizes[4];
    size_t out_size = launch != 0u ? sizeof output : sizeof output.decision;
    unsigned input_count = 3u;
    result.status = BOOT_RUNTIME_WATCHDOG_ARGUMENT;
    if (bridge == NULL) return result;
    if (atomic_flag_test_and_set(&bridge->entered))
    {
        result.status = BOOT_RUNTIME_WATCHDOG_BUSY;
        return result;
    }
    if (bridge->consumed != 0u)
    {
        result.status = BOOT_RUNTIME_WATCHDOG_CONSUMED;
        goto leave;
    }
    if (w == NULL || registers == NULL || store == NULL || images == NULL ||
        reader == NULL || out == NULL || registers->read == NULL ||
        registers->write == NULL || store->read == NULL ||
        store->erase_page == NULL || store->program_word == NULL) goto leave;
    if (overlaps(bridge, sizeof *bridge, w, sizeof *w) ||
        overlaps(out, out_size, bridge, sizeof *bridge) ||
        overlaps(out, out_size, w, sizeof *w)) goto leave;
    if (launch != 0u)
    {
        if (h == NULL || observed == NULL ||
            overlaps(h, sizeof *h, bridge, sizeof *bridge) ||
            overlaps(h, sizeof *h, w, sizeof *w) ||
            overlaps(h, sizeof *h, out, out_size)) goto leave;
        inputs[3] = observed; sizes[3] = sizeof *observed;
        input_count = 4u;
    }
    inputs[0] = registers; sizes[0] = sizeof *registers;
    inputs[1] = store; sizes[1] = sizeof *store;
    inputs[2] = images; sizes[2] = sizeof *images;
    for (unsigned i = 0u; i < input_count; ++i)
        if (overlaps(bridge, sizeof *bridge, inputs[i], sizes[i]) ||
            overlaps(w, sizeof *w, inputs[i], sizes[i]) ||
            overlaps(out, out_size, inputs[i], sizes[i]) ||
            (launch != 0u && overlaps(h, sizeof *h, inputs[i], sizes[i]))) goto leave;
    result.valid = BOOT_RUNTIME_WATCHDOG_DIAGNOSTIC;
    result.status = BOOT_RUNTIME_WATCHDOG_REJECTED;
    if (atomic_flag_test_and_set(&w->entered))
    {
        result.watchdog = BOOT_WATCHDOG_BUSY;
        goto leave;
    }
    /* STE: no reset, flash, configure or feed on invalid protection. */
    result.watchdog = w->blocked != 0u ? BOOT_WATCHDOG_BLOCKED :
        (w->configured == 0u || w->owner != BOOT_WATCHDOG_BOOT ||
         w->reload == 0u || w->epoch == 0u || w->io.read_register == NULL ||
         w->io.write_register == NULL ? BOOT_WATCHDOG_STATE : BOOT_WATCHDOG_OK);
    if (result.watchdog != BOOT_WATCHDOG_OK) goto release;
    if (launch != 0u)
    {
        result.valid |= BOOT_RUNTIME_WATCHDOG_HANDOFF;
        result.handoff = BOOT_HANDOFF_BUSY;
        if (atomic_flag_test_and_set(&h->entered)) goto release;
        observation = *observed;
        result.handoff = h->blocked != 0u ? BOOT_HANDOFF_BLOCKED :
            (h->finished != 0u ? BOOT_HANDOFF_STATE : BOOT_HANDOFF_ARGUMENT);
        if (h->blocked != 0u || h->finished != 0u ||
            h->io.operation == NULL || h->io.terminal_intent == NULL ||
            observation.ipsr != 0u || (observation.control & ~UINT32_C(2)) != 0u ||
            observation.primask > 1u || observation.faultmask > 1u ||
            (observation.basepri & ~UINT32_C(0xe0)) != 0u)
            goto release_handoff;
        result.handoff = BOOT_HANDOFF_OK;
    }
    reset_io = *registers;
    storage = *store;
    prefixes = *images;
    bridge->consumed = 1u;
    /* Discard preexisting evidence, not a bank for this transaction. */
    w->evidence = 0u;
    result.valid |= BOOT_RUNTIME_WATCHDOG_RUNTIME;
    result.runtime = boot_runtime_run(&bridge->runtime, &reset_io, &storage,
        &prefixes, reader, reader_context, progress, &context, &output.decision);
    if (result.runtime.status != BOOT_RUNTIME_OK)
    {
        result.status = BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED;
        goto quarantine;
    }
    if (launch != 0u)
    {
        request.image = output.decision.image;
        request.selected_slot = output.decision.image.manifest.slot;
        request.watchdog_epoch = w->epoch;
        request.observed = observation;
        result.handoff = boot_handoff_run_held(h, w, &request, &output.intent);
        if (result.handoff != BOOT_HANDOFF_OK)
        {
            result.status = BOOT_RUNTIME_WATCHDOG_HANDOFF_FAILED;
            goto quarantine;
        }
        *(boot_runtime_watchdog_launch_t *)out = output;
    }
    else *(boot_launch_decision_t *)out = output.decision;
    result.status = BOOT_RUNTIME_WATCHDOG_OK;
    goto release_handoff;
quarantine:
    /* No cleanup IO, feed, rollback or accounting replay. */
    w->blocked = 1u;
    w->evidence = 0u;
    if (launch != 0u) h->blocked = 1u;
release_handoff:
    if (launch != 0u) atomic_flag_clear(&h->entered);
release:
    atomic_flag_clear(&w->entered);
leave:
    atomic_flag_clear(&bridge->entered);
    return result;
}

boot_runtime_watchdog_result_t boot_runtime_watchdog_run(
    boot_runtime_watchdog_t *bridge, boot_watchdog_t *w,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_launch_decision_t *out)
{
    return run(bridge, w, NULL, NULL, 0u, registers, store, images,
        reader, reader_context, out);
}

boot_runtime_watchdog_result_t boot_runtime_watchdog_launch(
    boot_runtime_watchdog_t *bridge, boot_watchdog_t *w,
    boot_handoff_t *h, const boot_handoff_context_t *observed,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_runtime_watchdog_launch_t *out)
{
    return run(bridge, w, h, observed, 1u, registers, store, images,
        reader, reader_context, out);
}
