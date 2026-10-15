#include "lm3s6965/boot_watchdog.h"
#include "boot_watchdog_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_verify.h"

#define WDT_ENABLE UINT32_C(3)
#define WDT_CLOCK UINT32_C(8)
#define WDT_UNLOCK UINT32_C(0x1acce551)
#define APP_REQUIRED UINT32_C(7)

static boot_watchdog_result_t enter(boot_watchdog_t *w)
{
    if (w == NULL) return BOOT_WATCHDOG_ARGUMENT;
    if (atomic_flag_test_and_set_explicit(&w->entered, memory_order_acquire))
        return BOOT_WATCHDOG_BUSY;
    return BOOT_WATCHDOG_OK;
}

static boot_watchdog_result_t leave(boot_watchdog_t *w, boot_watchdog_result_t result)
{
    atomic_flag_clear_explicit(&w->entered, memory_order_release);
    return result;
}

static int rr(boot_watchdog_t *w, uint32_t address, uint32_t *value)
{
    return w->io.read_register(w->io.context, address, value);
}

static int wr(boot_watchdog_t *w, uint32_t address, uint32_t value)
{
    return w->io.write_register(w->io.context, address, value);
}

static boot_watchdog_result_t lock(boot_watchdog_t *w, uint32_t value, uint32_t expected)
{
    uint32_t status;
    if (wr(w, BOOT_WATCHDOG_LOCK, value) != 0 ||
        rr(w, BOOT_WATCHDOG_LOCK, &status) != 0) return BOOT_WATCHDOG_IO;
    return status == expected ? BOOT_WATCHDOG_OK : BOOT_WATCHDOG_VERIFY;
}

static boot_watchdog_result_t stop(boot_watchdog_t *w, boot_watchdog_result_t result)
{
    w->blocked = 1u;
    w->evidence = 0u;
    return leave(w, result);
}

static boot_watchdog_result_t fault(boot_watchdog_t *w, boot_watchdog_result_t result)
{
    /* STE: after attempted unlock, cleanup only; never retry CTL/LOAD/ICR
     * or disable an enabled timer. No unclocked register cleanup is attempted. */
    (void)lock(w, 0u, 1u);
    return stop(w, result);
}

static boot_watchdog_result_t ready(boot_watchdog_t *w,
                                    boot_watchdog_owner_t owner, uint32_t epoch)
{
    if (w->blocked != 0u) return BOOT_WATCHDOG_BLOCKED;
    if (w->configured == 0u || owner != w->owner || epoch != w->epoch)
        return BOOT_WATCHDOG_STATE;
    return BOOT_WATCHDOG_OK;
}

boot_watchdog_result_t boot_watchdog_init(boot_watchdog_t *w, const boot_watchdog_io_t *io)
{
    if (w == NULL || io == NULL || io->read_register == NULL || io->write_register == NULL)
        return BOOT_WATCHDOG_ARGUMENT;
    w->io = *io;
    w->entered = (atomic_flag)ATOMIC_FLAG_INIT;
    w->configured = w->blocked = 0u;
    w->owner = BOOT_WATCHDOG_BOOT;
    w->epoch = 1u;
    w->evidence = w->reload = w->verification = w->total = w->completed = 0u;
    w->phase = 0u;
    return BOOT_WATCHDOG_OK;
}

boot_watchdog_result_t boot_watchdog_configure(boot_watchdog_t *w,
                                               uint32_t hz, uint32_t reload)
{
    uint32_t value, control;
    boot_watchdog_result_t result;
    /* Caller-selected nominal ticks, not an elapsed time or LOAD+1 assertion. */
    if (hz == 0u || (uint64_t)hz * UINT64_C(5) != reload)
        return BOOT_WATCHDOG_ARGUMENT;
    result = enter(w);
    if (result != BOOT_WATCHDOG_OK) return result;
    if (w->blocked != 0u) return leave(w, BOOT_WATCHDOG_BLOCKED);
    if (w->configured != 0u) return leave(w, BOOT_WATCHDOG_STATE);
    if (rr(w, BOOT_WATCHDOG_RCGC0, &value) != 0) return stop(w, BOOT_WATCHDOG_IO);
    if ((value & WDT_CLOCK) == 0u) return stop(w, BOOT_WATCHDOG_VERIFY);
    if (rr(w, BOOT_WATCHDOG_CTL, &control) != 0) return stop(w, BOOT_WATCHDOG_IO);
    if ((control & UINT32_C(1)) != 0u) return stop(w, BOOT_WATCHDOG_STATE);
    result = lock(w, WDT_UNLOCK, 0u);
    if (result != BOOT_WATCHDOG_OK) return fault(w, result);
    if (wr(w, BOOT_WATCHDOG_LOAD, reload) != 0 ||
        rr(w, BOOT_WATCHDOG_LOAD, &value) != 0) return fault(w, BOOT_WATCHDOG_IO);
    if (value != reload) return fault(w, BOOT_WATCHDOG_VERIFY);
    /* LOAD does not clear a stale raw interrupt. Establish the first stage
     * before enabling protection, not a progress feed of a running controller. */
    if (wr(w, BOOT_WATCHDOG_ICR, 0u) != 0 ||
        rr(w, BOOT_WATCHDOG_RIS, &value) != 0) return fault(w, BOOT_WATCHDOG_IO);
    if ((value & UINT32_C(1)) != 0u) return fault(w, BOOT_WATCHDOG_VERIFY);
    /* STE: preserve reserved bits; RESEN and irreversible INTEN together. */
    if (wr(w, BOOT_WATCHDOG_CTL, control | WDT_ENABLE) != 0 ||
        rr(w, BOOT_WATCHDOG_CTL, &value) != 0) return fault(w, BOOT_WATCHDOG_IO);
    if ((value & WDT_ENABLE) != WDT_ENABLE) return fault(w, BOOT_WATCHDOG_VERIFY);
    result = lock(w, 0u, 1u);
    if (result != BOOT_WATCHDOG_OK) return fault(w, result);
    w->reload = reload;
    w->configured = 1u;
    return leave(w, BOOT_WATCHDOG_OK);
}

static boot_watchdog_result_t quarantine_held(boot_watchdog_t *w,
                                             boot_watchdog_result_t result)
{
    w->blocked = 1u;
    w->evidence = 0u;
    return result;
}

static boot_watchdog_result_t fault_held(boot_watchdog_t *w,
                                        boot_watchdog_result_t result)
{
    (void)lock(w, 0u, 1u);
    return quarantine_held(w, result);
}

boot_watchdog_result_t boot_watchdog_service_held(boot_watchdog_t *w,
    boot_watchdog_owner_t owner, uint32_t epoch)
{
    uint32_t value;
    boot_watchdog_result_t result = ready(w, owner, epoch);
    if (result != BOOT_WATCHDOG_OK) return result;
    if (w->evidence != (owner == BOOT_WATCHDOG_BOOT ? 1u : APP_REQUIRED))
        return BOOT_WATCHDOG_PROGRESS;
    if (w->epoch == UINT32_MAX) return quarantine_held(w, BOOT_WATCHDOG_STATE);
    /* Evidence is never recycled even if a subsequent operation is ambiguous. */
    w->evidence = 0u;
    if (rr(w, BOOT_WATCHDOG_RCGC0, &value) != 0) return quarantine_held(w, BOOT_WATCHDOG_IO);
    if ((value & WDT_CLOCK) == 0u) return quarantine_held(w, BOOT_WATCHDOG_VERIFY);
    if (rr(w, BOOT_WATCHDOG_CTL, &value) != 0) return quarantine_held(w, BOOT_WATCHDOG_IO);
    if ((value & WDT_ENABLE) != WDT_ENABLE) return quarantine_held(w, BOOT_WATCHDOG_VERIFY);
    if (rr(w, BOOT_WATCHDOG_LOAD, &value) != 0) return quarantine_held(w, BOOT_WATCHDOG_IO);
    if (value != w->reload) return quarantine_held(w, BOOT_WATCHDOG_VERIFY);
    result = lock(w, WDT_UNLOCK, 0u);
    if (result != BOOT_WATCHDOG_OK) return fault_held(w, result);
    /* ICR is WO/indeterminate: never read it or compare running VALUE to LOAD. */
    if (wr(w, BOOT_WATCHDOG_ICR, 0u) != 0 ||
        rr(w, BOOT_WATCHDOG_RIS, &value) != 0) return fault_held(w, BOOT_WATCHDOG_IO);
    if ((value & UINT32_C(1)) != 0u) return fault_held(w, BOOT_WATCHDOG_VERIFY);
    result = lock(w, 0u, 1u);
    if (result != BOOT_WATCHDOG_OK) return fault_held(w, result);
    ++w->epoch;
    return BOOT_WATCHDOG_OK;
}

boot_watchdog_result_t boot_watchdog_service(boot_watchdog_t *w,
    boot_watchdog_owner_t owner, uint32_t epoch)
{
    boot_watchdog_result_t result = enter(w);
    if (result != BOOT_WATCHDOG_OK) return result;
    return leave(w, boot_watchdog_service_held(w, owner, epoch));
}

boot_watchdog_result_t boot_watchdog_transfer(boot_watchdog_t *w, uint32_t epoch)
{
    boot_watchdog_result_t result = enter(w);
    if (result != BOOT_WATCHDOG_OK) return result;
    result = ready(w, BOOT_WATCHDOG_BOOT, epoch);
    if (result != BOOT_WATCHDOG_OK) return leave(w, result);
    if (w->epoch == UINT32_MAX) return stop(w, BOOT_WATCHDOG_STATE);
    w->owner = BOOT_WATCHDOG_APPLICATION;
    w->evidence = 0u;
    ++w->epoch;
    return leave(w, BOOT_WATCHDOG_OK);
}

boot_watchdog_result_t boot_watchdog_report_held(boot_watchdog_t *w,
    uint32_t epoch, uint32_t verification, boot_verify_phase_t phase,
    uint32_t completed, uint32_t total)
{
    boot_watchdog_result_t result = ready(w, BOOT_WATCHDOG_BOOT, epoch);
    if (result != BOOT_WATCHDOG_OK) return result;
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN)
    {
        if (verification == 0u || verification <= w->verification || completed != 0u ||
            total < BOOT_PAYLOAD_MIN || total > BOOT_PAYLOAD_MAX || total % 4u != 0u)
            return BOOT_WATCHDOG_PROGRESS;
        w->verification = verification;
        w->total = total;
        w->completed = w->evidence = 0u;
        w->phase = 1u;
        return BOOT_WATCHDOG_OK;
    }
    if (verification != w->verification || total != w->total || w->phase == 0u)
        return BOOT_WATCHDOG_PROGRESS;
    if (phase == BOOT_VERIFY_MANIFEST_AUTHENTICATED)
    {
        if (w->phase != 1u || completed != 0u) return BOOT_WATCHDOG_PROGRESS;
        w->phase = 2u;
    }
    else if (phase == BOOT_VERIFY_PAYLOAD_PROGRESS)
    {
        uint32_t remaining = total - w->completed;
        uint32_t chunk = remaining < BOOT_VERIFY_CHUNK_BYTES ? remaining : BOOT_VERIFY_CHUNK_BYTES;
        if (w->phase != 2u || remaining == 0u || completed <= w->completed ||
            completed > total || completed - w->completed != chunk)
            return BOOT_WATCHDOG_PROGRESS;
        w->completed = completed;
    }
    else return BOOT_WATCHDOG_PROGRESS;
    w->evidence = 1u;
    return BOOT_WATCHDOG_OK;
}

boot_watchdog_result_t boot_watchdog_boot_report(boot_watchdog_t *w,
    uint32_t epoch, uint32_t verification, boot_verify_phase_t phase,
    uint32_t completed, uint32_t total)
{
    boot_watchdog_result_t result = enter(w);
    if (result != BOOT_WATCHDOG_OK) return result;
    return leave(w, boot_watchdog_report_held(w, epoch, verification, phase, completed, total));
}

boot_watchdog_result_t boot_watchdog_application_report(boot_watchdog_t *w,
    uint32_t epoch, uint32_t predicate, unsigned complete)
{
    boot_watchdog_result_t result = enter(w);
    if (result != BOOT_WATCHDOG_OK) return result;
    result = ready(w, BOOT_WATCHDOG_APPLICATION, epoch);
    if (result != BOOT_WATCHDOG_OK) return leave(w, result);
    if (complete != 1u || (predicate != BOOT_WATCHDOG_ETHERNET &&
        predicate != BOOT_WATCHDOG_TIMERS && predicate != BOOT_WATCHDOG_FTP) ||
        (w->evidence & predicate) != 0u) return leave(w, BOOT_WATCHDOG_PROGRESS);
    w->evidence |= predicate;
    return leave(w, BOOT_WATCHDOG_OK);
}
