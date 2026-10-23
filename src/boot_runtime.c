#include "lm3s6965/boot_runtime.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

void boot_runtime_init(boot_runtime_t *runtime)
{
    if (runtime == NULL) return;
    atomic_flag_clear(&runtime->entered);
    runtime->consumed = 0u;
}

boot_runtime_result_t boot_runtime_run(boot_runtime_t *runtime,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out)
{
    boot_runtime_result_t result = {0};
    boot_reset_register_adapter_t reset_io;
    boot_store_adapter_t storage;
    boot_select_images_t prefixes;
    boot_launch_decision_t decision;
    result.status = BOOT_RUNTIME_ARGUMENT;
    if (runtime == NULL) return result;
    /* STE: Guard precedes admission and any inspection of mutable inputs. */
    if (atomic_flag_test_and_set(&runtime->entered))
    {
        result.status = BOOT_RUNTIME_BUSY;
        return result;
    }
    if (runtime->consumed != 0u)
    {
        result.status = BOOT_RUNTIME_CONSUMED;
        goto leave;
    }
    if (registers == NULL || store == NULL || images == NULL || reader == NULL ||
        out == NULL || registers->read == NULL || registers->write == NULL ||
        store->read == NULL || store->erase_page == NULL || store->program_word == NULL ||
        overlaps(runtime, sizeof *runtime, registers, sizeof *registers) ||
        overlaps(runtime, sizeof *runtime, store, sizeof *store) ||
        overlaps(runtime, sizeof *runtime, images, sizeof *images) ||
        overlaps(out, sizeof *out, runtime, sizeof *runtime) ||
        overlaps(out, sizeof *out, registers, sizeof *registers) ||
        overlaps(out, sizeof *out, store, sizeof *store) ||
        overlaps(out, sizeof *out, images, sizeof *images)) goto leave;
    reset_io = *registers;
    storage = *store;
    prefixes = *images;
    /* STE: Every admitted exit is terminal, including ambiguous effects. */
    runtime->consumed = 1u;
    result.valid = BOOT_RUNTIME_REPORT_RESET;
    result.reset_status = boot_reset_handler_run(&reset_io, &storage, &result.reset);
    if (result.reset_status != BOOT_RESET_HANDLER_OK)
    {
        result.status = (result.reset.valid & BOOT_RESET_REPORT_STORE_RESULT) != 0u &&
            result.reset.store_result == BOOT_STORE_UNCERTAIN ?
            BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_RESET_FAILED;
        goto leave;
    }
    result.valid |= BOOT_RUNTIME_REPORT_SELECT;
    result.selection = boot_select(&storage, &result.reset.snapshot, &prefixes,
        reader, reader_context, progress, progress_context, &decision);
    if (result.selection.status != BOOT_SELECT_OK)
    {
        result.status = result.selection.status == BOOT_SELECT_RECONCILE ?
            BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_SELECT_FAILED;
        goto leave;
    }
    *out = decision;
    result.status = BOOT_RUNTIME_OK;
leave:
    atomic_flag_clear(&runtime->entered);
    return result;
}
