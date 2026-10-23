#include "lm3s6965/boot_receive.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an && bn && (x <= y ? y - x < an : x - y < bn);
}
static int aliases(const boot_receive_t *r, const void *p, size_t n)
{
    return overlaps(r, sizeof *r, p, n) ||
        (r->phase != BOOT_RECEIVE_NEW &&
         (overlaps(r->config.flash, sizeof *r->config.flash, p, n) ||
          overlaps(r->config.running_context, r->config.running_context ? 1u : 0u, p, n) ||
          overlaps(r->config.progress_context, r->config.progress_context ? 1u : 0u, p, n)));
}
static boot_receive_result_t enter(boot_receive_t *r)
{
    if (!r) return BOOT_RECEIVE_ARGUMENT;
    if (atomic_flag_test_and_set_explicit(&r->entered, memory_order_acquire))
        return BOOT_RECEIVE_BUSY;
    return BOOT_RECEIVE_OK;
}
static boot_receive_result_t leave(boot_receive_t *r, boot_receive_result_t result)
{
    atomic_flag_clear_explicit(&r->entered, memory_order_release);
    return result;
}
static boot_receive_result_t stop(boot_receive_t *r, boot_receive_result_t result)
{
    r->failed_phase = r->phase;
    r->failure = result;
    r->phase = BOOT_RECEIVE_TERMINAL;
    return result;
}
static boot_receive_result_t store_failure(boot_receive_t *r)
{
    return stop(r, r->store_result == BOOT_STORE_UNCERTAIN || r->config.flash->blocked ?
                BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_STORE);
}
static boot_receive_result_t flash_failure(boot_receive_t *r)
{
    return stop(r, r->config.flash->blocked ? BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_FLASH);
}
/* Preserve the first physical failure across store's integer callback ABI. */
static int metadata_result(boot_receive_t *r, boot_flash_result_t result)
{
    if (r->flash_result == BOOT_FLASH_OK) r->flash_result = result;
    return result == BOOT_FLASH_OK ? 0 : -1;
}
static int metadata_read(void *context, uint32_t address, uint8_t *out, size_t n)
{
    boot_receive_t *r = context;
    return metadata_result(r, boot_flash_read(r->config.flash, address, out, n));
}
static int metadata_erase(void *context, uint32_t address)
{
    boot_receive_t *r = context;
    return metadata_result(r, boot_flash_erase(r->config.flash, address));
}
static int metadata_program(void *context, uint32_t address, uint32_t value)
{
    boot_receive_t *r = context;
    return metadata_result(r, boot_flash_program(r->config.flash, address, value));
}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static size_t read_payload(void *context, uint32_t address, uint8_t *out, size_t n)
{
    boot_receive_t *r = context;
    r->flash_result = boot_flash_read(r->config.flash, address, out, n);
    return r->flash_result == BOOT_FLASH_OK ? n : 0;
}
boot_receive_result_t boot_receive_begin(boot_receive_t *r, const boot_receive_config_t *c)
{
    boot_receive_result_t result = enter(r);
    if (result != BOOT_RECEIVE_OK) return result;
    if (r->phase != BOOT_RECEIVE_NEW)
        return leave(r, r->phase == BOOT_RECEIVE_TERMINAL ? BOOT_RECEIVE_CONSUMED : BOOT_RECEIVE_BUSY);
    if (!c || overlaps(r, sizeof *r, c, sizeof *c) || !c->flash || !c->running ||
        c->permitted_slot > BOOT_SLOT_B ||
        overlaps(r, sizeof *r, c->flash, sizeof *c->flash) ||
        overlaps(c, sizeof *c, c->flash, sizeof *c->flash) ||
        overlaps(r, sizeof *r, c->running_context, c->running_context ? 1u : 0u) ||
        overlaps(r, sizeof *r, c->progress_context, c->progress_context ? 1u : 0u))
        return leave(r, BOOT_RECEIVE_ARGUMENT);
    r->config = *c;
    r->adapter = (boot_store_adapter_t){ r, metadata_read, metadata_erase, metadata_program };
    r->flash_result = BOOT_FLASH_OK;
    r->phase = BOOT_RECEIVE_HEADER; /* consumed BEFORE any callback */
    r->store_result = boot_store_load(&r->adapter, &r->authority);
    if (r->store_result != BOOT_STORE_OK)
        return leave(r, stop(r, r->config.flash->blocked ?
            BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_AUTHORITY));
    if (r->authority.record.sequence > UINT32_MAX - 2u)
        return leave(r, stop(r, BOOT_RECEIVE_EXHAUSTED));
    if (r->config.running(r->config.running_context, &r->evidence.running)) {
        if (r->config.flash->blocked) {
            r->flash_result = r->config.flash->blocked_result;
            return leave(r, stop(r, BOOT_RECEIVE_RECONCILE));
        }
        return leave(r, stop(r, BOOT_RECEIVE_RUNNING));
    }
    r->evidence.permitted_slot = r->config.permitted_slot;
    return leave(r, BOOT_RECEIVE_OK);
}
boot_receive_result_t boot_receive_feed(boot_receive_t *r, const uint8_t *p, size_t n, size_t *accepted)
{
    boot_receive_result_t result = enter(r);
    if (result != BOOT_RECEIVE_OK) return result;
    if (!accepted || (!p && n) || aliases(r, accepted, sizeof *accepted) ||
        aliases(r, p, n) || overlaps(p, n, accepted, sizeof *accepted))
        return leave(r, BOOT_RECEIVE_ARGUMENT);
    *accepted = 0;
    if (r->phase == BOOT_RECEIVE_TERMINAL) return leave(r, BOOT_RECEIVE_CONSUMED);
    if (r->phase == BOOT_RECEIVE_NEW) return leave(r, BOOT_RECEIVE_ARGUMENT);
    if (!n) return leave(r, BOOT_RECEIVE_OK);
    if (r->eof || (r->phase >= BOOT_RECEIVE_PAYLOAD &&
        n > r->evidence.incoming.payload_length - r->received))
        return leave(r, stop(r, BOOT_RECEIVE_LENGTH));
    if (r->phase == BOOT_RECEIVE_HEADER) {
        size_t count = BOOT_MANIFEST_BYTES - r->manifest_used;
        if (count > n) count = n;
        memcpy(r->manifest + r->manifest_used, p, count);
        r->manifest_used += count; *accepted = count;
        if (r->manifest_used == BOOT_MANIFEST_BYTES) r->phase = BOOT_RECEIVE_AUTHENTICATE;
    } else if (r->phase == BOOT_RECEIVE_PAYLOAD) {
        size_t count = sizeof r->word - r->word_used;
        if (count > n) count = n;
        if (!count) return leave(r, BOOT_RECEIVE_WAIT);
        memcpy(r->word + r->word_used, p, count);
        r->word_used += count; r->received += (uint32_t)count; *accepted = count;
    } else return leave(r, BOOT_RECEIVE_WAIT);
    return leave(r, BOOT_RECEIVE_OK);
}
boot_receive_result_t boot_receive_eof(boot_receive_t *r)
{
    boot_receive_result_t result = enter(r);
    if (result != BOOT_RECEIVE_OK) return result;
    if (r->phase == BOOT_RECEIVE_TERMINAL) return leave(r, BOOT_RECEIVE_CONSUMED);
    if (r->phase == BOOT_RECEIVE_NEW) return leave(r, BOOT_RECEIVE_ARGUMENT);
    if (r->manifest_used != BOOT_MANIFEST_BYTES ||
        r->phase < BOOT_RECEIVE_PAYLOAD || r->received != r->evidence.incoming.payload_length)
        return leave(r, stop(r, BOOT_RECEIVE_LENGTH));
    r->eof = 1;
    return leave(r, BOOT_RECEIVE_OK);
}
boot_receive_result_t boot_receive_abort(boot_receive_t *r)
{
    boot_receive_result_t result = enter(r);
    if (result != BOOT_RECEIVE_OK) return result;
    if (r->phase == BOOT_RECEIVE_TERMINAL) return leave(r, BOOT_RECEIVE_CONSUMED);
    if (r->phase == BOOT_RECEIVE_NEW) return leave(r, BOOT_RECEIVE_ARGUMENT);
    return leave(r, stop(r, BOOT_RECEIVE_ABORTED));
}
boot_receive_result_t boot_receive_step(boot_receive_t *r, boot_receive_acceptance_t *out)
{
    boot_receive_result_t result = enter(r);
    if (result != BOOT_RECEIVE_OK) return result;
    if (!out || aliases(r, out, sizeof *out)) return leave(r, BOOT_RECEIVE_ARGUMENT);
    if (r->phase == BOOT_RECEIVE_TERMINAL) return leave(r, BOOT_RECEIVE_CONSUMED);
    switch (r->phase) {
    case BOOT_RECEIVE_NEW: result = BOOT_RECEIVE_ARGUMENT; break;
    case BOOT_RECEIVE_HEADER: result = BOOT_RECEIVE_WAIT; break;
    case BOOT_RECEIVE_AUTHENTICATE:
        r->verify_result = boot_verify_manifest(r->manifest, sizeof r->manifest,
            r->config.permitted_slot, r->config.progress, r->config.progress_context,
            &r->evidence.incoming);
        if (r->verify_result != BOOT_VERIFY_OK) { result = stop(r, BOOT_RECEIVE_MANIFEST); break; }
        r->lifecycle_result = boot_lifecycle_invalidate(&r->authority.record, &r->evidence, &r->proposal);
        if (r->lifecycle_result != BOOT_LIFECYCLE_CHANGE) { result = stop(r, BOOT_RECEIVE_ADMISSION); break; }
        r->phase = BOOT_RECEIVE_INVALIDATE;
        break;
    case BOOT_RECEIVE_INVALIDATE: {
        boot_store_snapshot_t committed;
        r->flash_result = BOOT_FLASH_OK;
        r->store_result = boot_store_replace(&r->adapter, &r->authority, &r->proposal, &committed);
        if (r->store_result != BOOT_STORE_OK) { result = store_failure(r); break; }
        r->authority = committed;
        r->flash_result = boot_flash_erase_begin(&r->erase, &r->authority,
            r->evidence.running.manifest.slot, r->config.permitted_slot);
        if (r->flash_result != BOOT_FLASH_OK) { result = flash_failure(r); break; }
        r->phase = BOOT_RECEIVE_ERASE;
        break;
    }
    case BOOT_RECEIVE_ERASE:
        r->flash_result = boot_flash_erase_step(r->config.flash, &r->erase);
        if (r->flash_result == BOOT_FLASH_DONE) r->phase = BOOT_RECEIVE_PAYLOAD;
        else if (r->flash_result != BOOT_FLASH_OK) result = flash_failure(r);
        break;
    case BOOT_RECEIVE_PAYLOAD:
        if (r->word_used == sizeof r->word) {
            r->flash_result = boot_flash_program(r->config.flash,
                r->evidence.incoming.execution_base + r->programmed, word(r->word));
            if (r->flash_result != BOOT_FLASH_OK) { result = flash_failure(r); break; }
            r->programmed += 4u; r->word_used = 0;
        } else result = BOOT_RECEIVE_WAIT;
        if (r->eof && r->programmed == r->evidence.incoming.payload_length) {
            r->phase = BOOT_RECEIVE_WRITE_MANIFEST; result = BOOT_RECEIVE_OK;
        }
        break;
    case BOOT_RECEIVE_WRITE_MANIFEST:
        r->flash_result = boot_flash_program(r->config.flash,
            (r->config.permitted_slot == BOOT_SLOT_A ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST) +
            r->manifest_programmed, word(r->manifest + r->manifest_programmed));
        if (r->flash_result != BOOT_FLASH_OK) { result = flash_failure(r); break; }
        r->manifest_programmed += 4u;
        if (r->manifest_programmed == BOOT_MANIFEST_BYTES) r->phase = BOOT_RECEIVE_VERIFY;
        break;
    case BOOT_RECEIVE_VERIFY: {
        uint8_t wire[BOOT_MANIFEST_BYTES];
        r->flash_result = boot_flash_read(r->config.flash,
            r->config.permitted_slot == BOOT_SLOT_A ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST,
            wire, sizeof wire);
        if (r->flash_result != BOOT_FLASH_OK) { result = flash_failure(r); break; }
        /* Exact readback, THEN real independent stored authentication/hash/vectors. */
        if (memcmp(wire, r->manifest, sizeof wire)) { result = stop(r, BOOT_RECEIVE_STORED); break; }
        r->verify_result = boot_verify_stored(wire, sizeof wire, r->config.permitted_slot,
            read_payload, r, r->config.progress, r->config.progress_context, &r->stored);
        if (r->verify_result != BOOT_VERIFY_OK) {
            result = stop(r, r->config.flash->blocked ? BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_STORED); break;
        }
        r->lifecycle_result = boot_lifecycle_accept_ready(&r->authority.record, &r->evidence,
            &r->stored, &r->proposal);
        if (r->lifecycle_result != BOOT_LIFECYCLE_CHANGE) { result = stop(r, BOOT_RECEIVE_ADMISSION); break; }
        r->phase = BOOT_RECEIVE_READY;
        break;
    }
    case BOOT_RECEIVE_READY: {
        boot_store_snapshot_t committed;
        r->flash_result = BOOT_FLASH_OK;
        r->store_result = boot_store_replace(&r->adapter, &r->authority, &r->proposal, &committed);
        if (r->store_result != BOOT_STORE_OK) { result = store_failure(r); break; }
        boot_receive_acceptance_t owned = { .metadata = committed, .image = r->stored, .reset_requested = 1 };
        r->authority = committed;
        r->phase = BOOT_RECEIVE_TERMINAL;
        *out = owned;
        result = BOOT_RECEIVE_ACCEPTED;
        break;
    }
    case BOOT_RECEIVE_TERMINAL: result = BOOT_RECEIVE_CONSUMED; break;
    }
    return leave(r, result);
}
