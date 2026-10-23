#include "lm3s6965/boot_watchdog.h"

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lm3s6965/boot_verify.h"

typedef struct
{
    uint32_t clock, ctl, load, ris, locked, value;
    unsigned calls, fail, after, ignored, feeds, enables, loads, persistent, initial_clears, resets;
    uint32_t corrupt;
    struct { uint32_t address, value; unsigned write; } trace[128];
    boot_watchdog_t *reenter;
    unsigned reentered, reenter_call;
} model_t;

static void reenter(model_t *m)
{
    boot_watchdog_t *w = m->reenter;
    if (w != NULL && m->reentered == 0u && m->calls == m->reenter_call)
    {
        unsigned char before[sizeof *w];
        unsigned calls = m->calls;
        memcpy(before, w, sizeof *w);
        m->reentered = 1u;
        /* Each guarded API must preserve the object AND the outer entry guard.
         * Compare after every call so later calls cannot mask a mutation. */
#define BUSY_UNCHANGED(call) do { \
        assert((call) == BOOT_WATCHDOG_BUSY); \
        assert(m->calls == calls && memcmp(before, w, sizeof *w) == 0); \
        assert(atomic_flag_test_and_set(&w->entered)); \
    } while (0)
        BUSY_UNCHANGED(boot_watchdog_service(w, w->owner, w->epoch));
        BUSY_UNCHANGED(boot_watchdog_configure(w, 100u, 500u));
        BUSY_UNCHANGED(boot_watchdog_transfer(w, w->epoch));
        BUSY_UNCHANGED(boot_watchdog_boot_report(w, w->epoch, 1u,
            BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u));
        BUSY_UNCHANGED(boot_watchdog_application_report(w, w->epoch, 1u, 1u));
#undef BUSY_UNCHANGED
    }
}

static int read_register(void *context, uint32_t address, uint32_t *value)
{
    model_t *m = context;
    assert(address == BOOT_WATCHDOG_RCGC0 || (m->clock & 8u) != 0u);
    assert(m->calls < sizeof m->trace / sizeof m->trace[0]);
    m->trace[m->calls].address = address;
    m->trace[m->calls].write = 0u;
    ++m->calls;
    reenter(m);
    if (m->calls == m->fail || (m->persistent != 0u && m->calls >= m->fail)) return -1;
    switch (address)
    {
        case BOOT_WATCHDOG_RCGC0: *value = m->clock; break;
        case BOOT_WATCHDOG_CTL: *value = m->ctl; break;
        case BOOT_WATCHDOG_LOAD: *value = m->load; break;
        case BOOT_WATCHDOG_RIS: *value = m->ris; break;
        case BOOT_WATCHDOG_LOCK: *value = m->locked; break;
        default: assert(0); return -1; /* ICR is NOT readable. */
    }
    if (address == m->corrupt) *value ^= 1u;
    return 0;
}

static int write_register(void *context, uint32_t address, uint32_t value)
{
    model_t *m = context;
    unsigned failed;
    assert((m->clock & 8u) != 0u);
    assert(m->calls < sizeof m->trace / sizeof m->trace[0]);
    m->trace[m->calls].address = address;
    m->trace[m->calls].value = value;
    m->trace[m->calls].write = 1u;
    ++m->calls;
    failed = m->calls == m->fail || (m->persistent != 0u && m->calls >= m->fail);
    reenter(m);
    if (failed != 0u && m->after == 0u) return -1;
    if (m->ignored == m->calls) return 0;
    if (address == BOOT_WATCHDOG_LOCK)
        m->locked = value == UINT32_C(0x1acce551) ? 0u : 1u;
    else if (m->locked == 0u)
    {
        switch (address)
        {
            case BOOT_WATCHDOG_CTL:
                /* Real hardware ignores ALL CTL writes once INTEN is set. */
                if ((m->ctl & 1u) == 0u)
                {
                    m->ctl = value;
                    ++m->enables;
                    m->value = m->load;
                }
                break;
            case BOOT_WATCHDOG_LOAD:
                m->load = m->value = value; ++m->loads; break;
            case BOOT_WATCHDOG_ICR:
                m->ris = 0u; m->value = m->load;
                if ((m->ctl & 1u) != 0u) ++m->feeds;
                else ++m->initial_clears;
                break;
            default: assert(0); return -1;
        }
    }
    return failed != 0u ? -1 : 0;
}

static void fresh(model_t *m, boot_watchdog_t *w)
{
    boot_watchdog_io_t io = {m, read_register, write_register};
    memset(m, 0, sizeof *m);
    m->clock = 8u;
    m->load = UINT32_MAX;
    m->locked = 1u;
    assert(boot_watchdog_init(w, &io) == BOOT_WATCHDOG_OK);
}

static void configured(model_t *m, boot_watchdog_t *w)
{
    fresh(m, w);
    assert(boot_watchdog_configure(w, 100u, 500u) == BOOT_WATCHDOG_OK);
    assert(m->ctl == 3u && m->locked == 1u && m->load == 500u);
    assert(m->enables == 1u && m->loads == 1u && m->feeds == 0u && m->initial_clears == 1u);
}

static void evidence(boot_watchdog_t *w)
{
    assert(boot_watchdog_boot_report(w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_OK);
}

static void quarantined(model_t *m, boot_watchdog_t *w)
{
    unsigned calls = m->calls;
    assert(w->blocked == 1u && w->evidence == 0u);
    assert(boot_watchdog_configure(w, 100u, 500u) == BOOT_WATCHDOG_BLOCKED);
    assert(boot_watchdog_service(w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_BLOCKED);
    assert(boot_watchdog_transfer(w, 1u) == BOOT_WATCHDOG_BLOCKED);
    assert(boot_watchdog_boot_report(w, 1u, 2u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_BLOCKED);
    assert(boot_watchdog_application_report(w, 1u, 1u, 1u) == BOOT_WATCHDOG_BLOCKED);
    assert(m->calls == calls);
}

static void failure_trace(const model_t *m, unsigned cleanup)
{
    unsigned i;
    assert(m->calls == m->fail + cleanup);
    /* STE: once a callback reports failure, only best-effort LOCK cleanup
     * is permitted: never retry LOAD, CTL or ICR, even after an effect. */
    for (i = m->fail; i < m->calls; ++i)
    {
        assert(m->trace[i].address == BOOT_WATCHDOG_LOCK);
        if (m->trace[i].write != 0u) assert(m->trace[i].value == 0u);
    }
}

static unsigned service_feeds(unsigned boundary, unsigned after)
{
    /* Service boundary 6 is the sole ICR write. A read failure has no effect;
     * an ICR callback may fail either before or after the hardware feed. */
    return boundary > 6u || (boundary == 6u && after != 0u) ? 1u : 0u;
}

static void registers(void)
{
    model_t m;
    boot_watchdog_t w;
    unsigned configure_calls, service_calls, i, after;
    configured(&m, &w);
    configure_calls = m.calls;
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_STATE);
    assert(m.calls == configure_calls);
    evidence(&w);
    m.ris = 1u; /* First stage can be cleared by genuine progress. */
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_OK);
    service_calls = m.calls - configure_calls;
    assert(configure_calls == 12u && service_calls == 9u);
    assert(m.trace[configure_calls + 5u].write == 1u &&
        m.trace[configure_calls + 5u].address == BOOT_WATCHDOG_ICR);
    assert(m.feeds == 1u && m.ris == 0u && m.locked == 1u);
    assert(w.epoch == 2u && w.evidence == 0u && m.enables == 1u && m.loads == 1u);
    for (after = 0u; after <= 1u; ++after)
    {
        for (i = 1u; i <= configure_calls; ++i)
        {
            fresh(&m, &w); m.fail = i; m.after = after;
            assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_IO);
            quarantined(&m, &w);
            assert(w.configured == 0u && m.feeds == 0u);
            assert(m.initial_clears == (i > 7u || (i == 7u && after != 0u) ? 1u : 0u));
            failure_trace(&m, i >= 3u ? 2u : 0u);
        }
        for (i = 1u; i <= service_calls; ++i)
        {
            configured(&m, &w); evidence(&w);
            m.fail = m.calls + i; m.after = after;
            assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_IO);
            quarantined(&m, &w);
            assert(w.epoch == 1u && m.ctl == 3u && m.feeds == service_feeds(i, after));
            failure_trace(&m, i >= 4u ? 2u : 0u);
        }
    }
    /* Readback is real LOCK status, LOAD, CTL and RIS, never ICR/VALUE. */
    {
        const uint32_t corrupt[] = {BOOT_WATCHDOG_LOCK, BOOT_WATCHDOG_LOAD, BOOT_WATCHDOG_CTL};
        for (i = 0u; i < sizeof corrupt / sizeof corrupt[0]; ++i)
        {
            fresh(&m, &w); m.corrupt = corrupt[i];
            assert(boot_watchdog_configure(&w, 100u, 500u) != BOOT_WATCHDOG_OK);
            quarantined(&m, &w);
        }
    }
    configured(&m, &w); evidence(&w); m.corrupt = BOOT_WATCHDOG_RIS;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_VERIFY);
    quarantined(&m, &w);
    fresh(&m, &w); m.clock = 0u;
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_VERIFY);
    quarantined(&m, &w);
    assert(m.calls == 1u && m.trace[0].address == BOOT_WATCHDOG_RCGC0 &&
        m.trace[0].write == 0u); /* No unclocked cleanup. */
    for (i = 1u; i <= 3u; i += 2u)
    {
        fresh(&m, &w); m.ctl = i;
        assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_STATE);
        quarantined(&m, &w);
        assert(m.ctl == i && m.loads == 0u);
    }
    fresh(&m, &w); m.ctl = UINT32_C(0x80000002); m.reenter = &w; m.reenter_call = 3u;
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_OK);
    assert(m.ctl == UINT32_C(0x80000003) && m.reentered == 1u);
    /* Failed relock may leave hardware unlocked: never claim otherwise. */
    configured(&m, &w); evidence(&w);
    m.fail = m.calls + service_calls - 1u;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_IO);
    quarantined(&m, &w);
    assert(m.ctl == 3u);
}

static void reentry(void)
{
    model_t m;
    boot_watchdog_t w;
    unsigned i, base;
    /* Every configure/read/write boundary, including the initial clock read. */
    for (i = 1u; i <= 12u; ++i)
    {
        fresh(&m, &w); m.reenter = &w; m.reenter_call = i;
        assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_OK);
        assert(m.reentered == 1u && m.calls == 12u && w.configured == 1u);
    }
    for (i = 1u; i <= 9u; ++i)
    {
        configured(&m, &w); evidence(&w); base = m.calls;
        m.reenter = &w; m.reenter_call = base + i;
        assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_OK);
        assert(m.reentered == 1u && m.calls == base + 9u && m.feeds == 1u && w.epoch == 2u);
    }
    /* Fail LOAD/ICR before effect, inject on cleanup LOCK write and read. */
    for (i = 1u; i <= 2u; ++i)
    {
        fresh(&m, &w); m.fail = 5u;
        m.reenter = &w; m.reenter_call = m.fail + i;
        assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_IO);
        assert(m.reentered == 1u && m.calls == 7u && m.locked == 1u);
        quarantined(&m, &w);
        configured(&m, &w); evidence(&w); m.fail = m.calls + 6u;
        m.reenter = &w; m.reenter_call = m.fail + i;
        assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_IO);
        assert(m.reentered == 1u && m.calls == m.fail + 2u && m.locked == 1u);
        quarantined(&m, &w);
    }
}

static void progress(void)
{
    model_t m;
    boot_watchdog_t w;
    unsigned calls, mask, bit;
    fresh(&m, &w);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_transfer(&w, 1u) == BOOT_WATCHDOG_STATE);
    assert(m.calls == 0u);
    configured(&m, &w); calls = m.calls;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 0u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 1u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 4u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 513u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_PROGRESS);
    /* No callback inside ECDSA: BEGIN alone provides no progress. */
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 2u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 768u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 1u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(m.calls == calls);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 1u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_application_report(&w, 1u, 1u, 1u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_OK);
    calls = m.calls;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 2u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 512u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 1u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 255u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 257u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(m.calls == calls);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 2u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 128u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 513u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 512u, 512u) == BOOT_WATCHDOG_OK);
    /* Abandon/restart requires a new ID; BEGIN discards prior evidence. */
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 3u, 2u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 3u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 512u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 3u, 2u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_OK);
    calls = m.calls;
    assert(boot_watchdog_transfer(&w, 2u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_transfer(&w, 3u) == BOOT_WATCHDOG_OK);
    assert(m.calls == calls && m.ctl == 3u && m.locked == 1u && w.evidence == 0u);
    assert(w.owner == BOOT_WATCHDOG_APPLICATION && w.epoch == 4u);
    assert(boot_watchdog_transfer(&w, 4u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 4u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_boot_report(&w, 4u, 3u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_STATE);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 4u) == BOOT_WATCHDOG_PROGRESS);
    assert(m.calls == calls);
    /* All subsets: no traffic counter exists; idle completed work is valid. */
    for (mask = 0u; mask < 8u; ++mask)
    {
        configured(&m, &w);
        assert(boot_watchdog_transfer(&w, 1u) == BOOT_WATCHDOG_OK);
        for (bit = 1u; bit <= 4u; bit <<= 1)
        {
            if ((mask & bit) == 0u) continue;
            assert(boot_watchdog_application_report(&w, 2u, bit, 0u) == BOOT_WATCHDOG_PROGRESS);
            assert(boot_watchdog_application_report(&w, 2u, bit, 1u) == BOOT_WATCHDOG_OK);
            assert(boot_watchdog_application_report(&w, 2u, bit, 1u) == BOOT_WATCHDOG_PROGRESS);
        }
        calls = m.calls;
        assert(boot_watchdog_application_report(&w, 2u, 1u, 2u) == BOOT_WATCHDOG_PROGRESS);
        assert(boot_watchdog_application_report(&w, 2u, 7u, 1u) == BOOT_WATCHDOG_PROGRESS);
        assert(boot_watchdog_application_report(&w, 2u, 0u, 1u) == BOOT_WATCHDOG_PROGRESS);
        assert(boot_watchdog_application_report(&w, 2u, 8u, 1u) == BOOT_WATCHDOG_PROGRESS);
        assert(boot_watchdog_application_report(&w, 1u, 1u, 1u) == BOOT_WATCHDOG_STATE);
        assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 2u) ==
            (mask == 7u ? BOOT_WATCHDOG_OK : BOOT_WATCHDOG_PROGRESS));
        if (mask != 7u) assert(m.calls == calls && w.evidence == mask);
        else
        {
            assert(w.epoch == 3u && w.evidence == 0u);
            assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 3u) == BOOT_WATCHDOG_PROGRESS);
            assert(boot_watchdog_application_report(&w, 2u, 1u, 1u) == BOOT_WATCHDOG_STATE);
            /* Confirmed application uses the identical policy indefinitely. */
            for (bit = 1u; bit <= 4u; bit <<= 1)
                assert(boot_watchdog_application_report(&w, 3u, bit, 1u) == BOOT_WATCHDOG_OK);
            assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 3u) == BOOT_WATCHDOG_OK);
            assert(m.ctl == 3u && m.feeds == 2u);
        }
    }
}

/* Logical expiration events only: no tick-period/cycle-exact assertion. */
static void zero_event(model_t *m)
{
    if ((m->ctl & 1u) == 0u) return;
    if (m->ris != 0u && (m->ctl & 2u) != 0u) ++m->resets;
    else { m->ris = 1u; m->value = m->load; }
}

static void application_faults(void)
{
    model_t m;
    boot_watchdog_t w;
    unsigned i, after, predicate, calls;
    for (after = 0u; after <= 1u; ++after)
    {
        for (i = 1u; i <= 9u; ++i)
        {
            configured(&m, &w);
            assert(boot_watchdog_transfer(&w, 1u) == BOOT_WATCHDOG_OK);
            for (predicate = 1u; predicate <= 4u; predicate <<= 1)
                assert(boot_watchdog_application_report(&w, 2u, predicate, 1u) == BOOT_WATCHDOG_OK);
            m.fail = m.calls + i; m.after = after;
            assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 2u) == BOOT_WATCHDOG_IO);
            quarantined(&m, &w);
            calls = m.calls;
            assert(boot_watchdog_service(&w, BOOT_WATCHDOG_APPLICATION, 2u) == BOOT_WATCHDOG_BLOCKED);
            assert(m.calls == calls && w.epoch == 2u && w.owner == BOOT_WATCHDOG_APPLICATION);
            assert(m.ctl == 3u && m.feeds == service_feeds(i, after));
            failure_trace(&m, i >= 4u ? 2u : 0u);
        }
    }
}

static void final_chunk(void)
{
    model_t m;
    boot_watchdog_t w;
    configured(&m, &w);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 492u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 492u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 1u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 256u, 492u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 2u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 488u, 492u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, 2u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 492u, 492u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 2u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, 3u, 1u, BOOT_VERIFY_PAYLOAD_PROGRESS, 492u, 492u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 3u) == BOOT_WATCHDOG_PROGRESS);
}

static void stages(void)
{
    model_t m;
    boot_watchdog_t w;
    unsigned calls;
    configured(&m, &w);
    zero_event(&m);
    assert(m.ris == 1u && m.value == 500u && m.resets == 0u);
    calls = m.calls;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_PROGRESS);
    assert(m.calls == calls && m.ris == 1u);
    zero_event(&m);
    assert(m.resets == 1u && m.feeds == 0u);
    configured(&m, &w); evidence(&w);
    zero_event(&m);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_OK);
    zero_event(&m);
    assert(m.resets == 0u && m.ris == 1u);
    zero_event(&m);
    assert(m.resets == 1u);
}

static void limits(void)
{
    model_t m;
    boot_watchdog_t w;
    boot_watchdog_io_t bad = {NULL, NULL, NULL};
    unsigned i, calls;
    const uint32_t service_corrupt[] = {BOOT_WATCHDOG_LOAD, BOOT_WATCHDOG_CTL, BOOT_WATCHDOG_LOCK};
    assert(boot_watchdog_init(NULL, &bad) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_init(&w, NULL) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_init(&w, &bad) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_configure(NULL, 100u, 500u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_service(NULL, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_transfer(NULL, 1u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_boot_report(NULL, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_application_report(NULL, 1u, 1u, 1u) == BOOT_WATCHDOG_ARGUMENT);
    fresh(&m, &w);
    assert(boot_watchdog_configure(&w, 0u, 0u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_configure(&w, 100u, 0u) == BOOT_WATCHDOG_ARGUMENT);
    assert(boot_watchdog_configure(&w, 100u, 1000u) == BOOT_WATCHDOG_ARGUMENT); /* Not 10s per stage. */
    assert(boot_watchdog_configure(&w, UINT32_MAX, UINT32_MAX) == BOOT_WATCHDOG_ARGUMENT);
    assert(m.calls == 0u);
    assert(boot_watchdog_configure(&w, UINT32_MAX / 5u, UINT32_MAX) == BOOT_WATCHDOG_OK);
    for (i = 0u; i < sizeof service_corrupt / sizeof service_corrupt[0]; ++i)
    {
        configured(&m, &w); evidence(&w); m.corrupt = service_corrupt[i];
        assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_VERIFY);
        quarantined(&m, &w);
    }
    /* Silent write loss detectable through useful readbacks. */
    for (i = 3u; i <= 11u; ++i)
    {
        if (i != 3u && i != 5u && i != 9u && i != 11u) continue;
        fresh(&m, &w); m.ignored = i;
        assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_VERIFY);
        quarantined(&m, &w);
    }
    fresh(&m, &w); m.ris = 1u; m.ignored = 7u;
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_VERIFY);
    quarantined(&m, &w);
    fresh(&m, &w); m.ris = 1u;
    assert(boot_watchdog_configure(&w, 100u, 500u) == BOOT_WATCHDOG_OK);
    assert(m.ris == 0u && m.initial_clears == 1u);
    configured(&m, &w); evidence(&w); m.ris = 1u; m.ignored = m.calls + 6u;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_VERIFY);
    quarantined(&m, &w);
    /* Persistent failure: cleanup cannot prove lock, enabled state remains. */
    configured(&m, &w); evidence(&w); m.persistent = 1u;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_IO);
    quarantined(&m, &w);
    assert(m.ctl == 3u);
    configured(&m, &w); evidence(&w);
    m.fail = m.calls + 8u; m.persistent = 1u;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_IO);
    quarantined(&m, &w);
    assert(m.ctl == 3u && m.feeds == 1u && m.locked == 0u);
    configured(&m, &w); evidence(&w); m.clock = 0u;
    calls = m.calls;
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, 1u) == BOOT_WATCHDOG_VERIFY);
    quarantined(&m, &w);
    assert(m.calls == calls + 1u && m.trace[calls].address == BOOT_WATCHDOG_RCGC0 &&
        m.trace[calls].write == 0u); /* No unclocked cleanup. */
    /* Exhaustion is synthetic, not permission for application field mutation. */
    configured(&m, &w); w.epoch = UINT32_MAX;
    assert(boot_watchdog_boot_report(&w, UINT32_MAX, UINT32_MAX, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&w, UINT32_MAX, 1u, BOOT_VERIFY_MANIFEST_BEGIN, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(boot_watchdog_boot_report(&w, UINT32_MAX, UINT32_MAX, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u, 512u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_service(&w, BOOT_WATCHDOG_BOOT, UINT32_MAX) == BOOT_WATCHDOG_STATE);
    assert(w.blocked == 1u && w.epoch == UINT32_MAX && m.feeds == 0u);
    configured(&m, &w); w.epoch = UINT32_MAX;
    assert(boot_watchdog_transfer(&w, UINT32_MAX) == BOOT_WATCHDOG_STATE);
    assert(w.owner == BOOT_WATCHDOG_BOOT && w.blocked == 1u);
    /* Model register semantics separately, without cycle-exact claims. */
    configured(&m, &w);
    assert(write_register(&m, BOOT_WATCHDOG_LOCK, UINT32_C(0x1acce551)) == 0);
    assert(write_register(&m, BOOT_WATCHDOG_CTL, 0u) == 0);
    assert(m.ctl == 3u); /* All CTL updates frozen. */
    m.ris = 1u;
    assert(write_register(&m, BOOT_WATCHDOG_LOAD, 400u) == 0);
    assert(m.ris == 1u); /* LOAD alone is NOT service. */
    assert(write_register(&m, BOOT_WATCHDOG_ICR, UINT32_MAX) == 0);
    assert(m.ris == 0u && m.value == 400u); /* ANY ICR value. */
    calls = m.calls;
    assert(boot_watchdog_boot_report(&w, 1u, 1u, (boot_verify_phase_t)99, 0u, 512u) == BOOT_WATCHDOG_PROGRESS);
    assert(m.calls == calls);
}

int main(void)
{
    registers();
    reentry();
    progress();
    limits();
    stages();
    final_chunk();
    application_faults();
    puts("watchdog register, ownership and progress tests passed");
    return 0;
}
