#include "lm3s6965/boot_reset_handler.h"

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

boot_reset_handler_result_t boot_reset_handler_run(
    const boot_reset_register_adapter_t *r, const boot_store_adapter_t *s,
    boot_reset_handler_report_t *report)
{
    boot_reset_handler_report_t p = {0};
    boot_store_snapshot_t loaded, committed;
    boot_metadata_t desired;
    uint32_t captured, fresh;
    if (r == NULL || s == NULL || report == NULL || r->read == NULL ||
        r->write == NULL || s->read == NULL || s->erase_page == NULL ||
        s->program_word == NULL ||
        overlaps(report, sizeof *report, r, sizeof *r) ||
        overlaps(report, sizeof *report, s, sizeof *s))
        return BOOT_RESET_HANDLER_ARGUMENT;
    if (r->read(r->context, &captured) != 0)
    { *report = p; return BOOT_RESET_HANDLER_CAPTURE_FAILED; }
    p.captured_flags = captured;
    p.valid = BOOT_RESET_REPORT_CAPTURE;
    if ((captured & BOOT_RESET_SUPPORTED) != 0u)
    {
        if (r->read(r->context, &fresh) != 0)
        { *report = p; return BOOT_RESET_HANDLER_CLEAR_READ_FAILED; }
        if (r->write(r->context, fresh & ~(captured & BOOT_RESET_SUPPORTED)) != 0)
        { *report = p; return BOOT_RESET_HANDLER_CLEAR_WRITE_FAILED; }
    }
    p.valid |= BOOT_RESET_REPORT_CLEAR;
    p.store_result = boot_store_load(s, &loaded);
    p.valid |= BOOT_RESET_REPORT_STORE_RESULT;
    if (p.store_result != BOOT_STORE_OK)
    {
        *report = p;
        return p.store_result == BOOT_STORE_NO_RECORD || p.store_result == BOOT_STORE_CONFLICT ||
            p.store_result == BOOT_STORE_INVALID_RECORD ? BOOT_RESET_HANDLER_RECOVERY :
            BOOT_RESET_HANDLER_STORE_FAILED;
    }
    p.lifecycle_result = boot_reset_propose(&loaded.record, captured, &desired, &p.decision);
    p.valid |= BOOT_RESET_REPORT_LIFECYCLE_RESULT;
    if (p.lifecycle_result != BOOT_LIFECYCLE_CHANGE &&
        p.lifecycle_result != BOOT_LIFECYCLE_NO_CHANGE)
    { *report = p; return BOOT_RESET_HANDLER_CLASSIFY_FAILED; }
    p.valid |= BOOT_RESET_REPORT_DECISION;
    if (p.lifecycle_result == BOOT_LIFECYCLE_CHANGE)
    {
        p.store_result = boot_store_replace(s, &loaded, &desired, &committed);
        if (p.store_result != BOOT_STORE_OK)
        { *report = p; return BOOT_RESET_HANDLER_STORE_FAILED; }
        p.snapshot = committed;
    }
    else p.snapshot = loaded;
    p.valid |= BOOT_RESET_REPORT_SNAPSHOT;
    *report = p;
    return BOOT_RESET_HANDLER_OK;
}
