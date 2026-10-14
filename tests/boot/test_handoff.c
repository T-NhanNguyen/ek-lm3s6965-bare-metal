#include "lm3s6965/boot_handoff.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

static boot_handoff_request_t request(uint32_t slot)
{
    boot_handoff_request_t r = {0};
    r.selected_slot = r.image.manifest.slot = slot;
    r.image.manifest.execution_base = slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    r.image.manifest.payload_length = BOOT_PAYLOAD_MIN;
    r.image.initial_msp = BOOT_INITIAL_MSP;
    r.image.reset_vector = r.image.manifest.execution_base | 1u;
    r.watchdog_epoch = 1u;
    return r;
}

static void run_rejected(const boot_handoff_request_t *r);

static void rejected(const boot_handoff_request_t *r)
{
    boot_handoff_intent_t out, before;
    memset(&out, 0xa5, sizeof out);
    before = out;
    assert(boot_handoff_admit(r, &out) == BOOT_HANDOFF_ARGUMENT);
    assert(memcmp(&out, &before, sizeof out) == 0);
    run_rejected(r);
}

static void admission(void)
{
    uint32_t slot;
    for (slot = BOOT_SLOT_A; slot <= BOOT_SLOT_B; ++slot)
    {
        boot_handoff_request_t good = request(slot), r;
        boot_handoff_intent_t out;
        uint32_t lengths[] = {236u, 240u, 111612u, 111616u};
        size_t i;
        for (i = 0u; i < sizeof lengths / sizeof lengths[0]; ++i)
        {
            good.image.manifest.payload_length = lengths[i];
            assert(boot_handoff_admit(&good, &out) == BOOT_HANDOFF_OK);
            assert(out.vtor == good.image.manifest.execution_base);
            assert(out.msp == BOOT_INITIAL_MSP && out.reset_vector == (out.vtor | 1u));
            assert(out.control == 0u && out.primask == 0u && out.basepri == 0u && out.faultmask == 0u);
            r = good; r.image.reset_vector = (out.vtor + lengths[i] - 2u) | 1u;
            assert(boot_handoff_admit(&r, &out) == BOOT_HANDOFF_OK);
            r.image.reset_vector = (good.image.manifest.execution_base + lengths[i]) | 1u;
            rejected(&r);
        }
        good = request(slot);
#define BAD(field, value) do { r = good; r.field = (value); rejected(&r); } while (0)
        BAD(selected_slot, BOOT_SLOT_NONE);
        BAD(image.manifest.slot, slot ^ 1u);
        BAD(image.manifest.execution_base, good.image.manifest.execution_base + 256u);
        BAD(image.manifest.execution_base, good.image.manifest.execution_base + 4u);
        BAD(image.manifest.payload_length, 0u);
        BAD(image.manifest.payload_length, 232u);
        BAD(image.manifest.payload_length, 237u);
        BAD(image.manifest.payload_length, 111620u);
        BAD(image.manifest.payload_length, UINT32_MAX);
        BAD(image.initial_msp, BOOT_INITIAL_MSP - 8u);
        BAD(image.initial_msp, BOOT_INITIAL_MSP + 8u);
        BAD(image.reset_vector, good.image.manifest.execution_base);
        BAD(image.reset_vector, (good.image.manifest.execution_base - 2u) | 1u);
        BAD(image.reset_vector, UINT32_MAX);
        BAD(observed.ipsr, 1u);
        BAD(observed.control, 1u);
        BAD(observed.control, 3u);
        BAD(observed.control, 4u);
        BAD(observed.control, UINT32_MAX);
        BAD(observed.primask, 2u);
        BAD(observed.basepri, 256u);
        BAD(observed.basepri, 1u);
        BAD(observed.basepri, 255u);
        BAD(observed.faultmask, 2u);
        BAD(watchdog_epoch, 0u);
        BAD(watchdog_epoch, UINT32_MAX);
#undef BAD
        good.observed.control = 2u;
        good.observed.primask = good.observed.faultmask = 1u;
        good.observed.basepri = 224u;
        good.watchdog_epoch = UINT32_MAX - 1u;
        assert(boot_handoff_admit(&good, &out) == BOOT_HANDOFF_OK);
        assert(out.application_epoch == UINT32_MAX);
        {
            uint32_t priority, control, masks;
            for (priority = 0u; priority < 8u; ++priority)
            for (control = 0u; control <= 2u; control += 2u)
            for (masks = 0u; masks < 4u; ++masks)
            {
                good.observed.basepri = priority << 5u;
                good.observed.control = control;
                good.observed.primask = masks & 1u;
                good.observed.faultmask = masks >> 1u;
                assert(boot_handoff_admit(&good, &out) == BOOT_HANDOFF_OK);
            }
        }
    }
    assert(boot_handoff_admit(NULL, NULL) == BOOT_HANDOFF_ARGUMENT);
}

typedef struct
{
    boot_handoff_t h;
    boot_watchdog_t w;
    boot_handoff_request_t r;
    boot_handoff_intent_t recorded;
    const boot_handoff_intent_t *output;
    boot_handoff_intent_t sentinel;
    unsigned calls, effects, watchdog_io, excluded, stopped, quiescent;
    unsigned disabled[2], pending[2], core_pending, terminal;
    uint32_t vtor, load, ctl, lock;
    unsigned fail, after, reenter, mutate;
} model_t;

static void observe_output(model_t *m, boot_handoff_intent_t *out)
{
    /* Output is separate from the model (including callback context/request). */
    memset(out, 0xa5, sizeof *out);
    m->output = out;
    m->sentinel = *out;
}

static void output_unchanged(const model_t *m)
{
    assert(m->output != NULL);
    assert(memcmp(m->output, &m->sentinel, sizeof m->sentinel) == 0);
}

static int read_register(void *context, uint32_t address, uint32_t *value)
{
    model_t *m = context;
    ++m->watchdog_io;
    switch (address)
    {
        case BOOT_WATCHDOG_RCGC0: *value = 8u; break;
        case BOOT_WATCHDOG_CTL: *value = m->ctl; break;
        case BOOT_WATCHDOG_LOAD: *value = m->load; break;
        case BOOT_WATCHDOG_LOCK: *value = m->lock; break;
        case BOOT_WATCHDOG_RIS: *value = 0u; break;
        default: assert(0); break;
    }
    return 0;
}

static int write_register(void *context, uint32_t address, uint32_t value)
{
    model_t *m = context;
    ++m->watchdog_io;
    switch (address)
    {
        case BOOT_WATCHDOG_CTL: m->ctl = value; break;
        case BOOT_WATCHDOG_LOAD: m->load = value; break;
        case BOOT_WATCHDOG_LOCK: m->lock = value == 0u ? 1u : 0u; break;
        case BOOT_WATCHDOG_ICR: assert(m->w.configured == 0u); break;
        default: assert(0); break;
    }
    return 0;
}

static void nested(model_t *m)
{
    boot_handoff_intent_t out, before;
    if (m->reenter == 0u) return;
    memset(&out, 0xa5, sizeof out); before = out;
    assert(boot_handoff_run(&m->h, &m->w, &m->r, &out) == BOOT_HANDOFF_BUSY);
    assert(memcmp(&out, &before, sizeof out) == 0);
    assert(m->h.blocked == 0u && m->h.finished == 0u);
}

static int operation(void *context, boot_handoff_operation_t op, uint32_t value)
{
    model_t *m = context;
    static const boot_handoff_operation_t expected[] = {
        BOOT_HANDOFF_EXCLUDE,
        BOOT_HANDOFF_STOP_SYSTICK,
        BOOT_HANDOFF_DISABLE_BANK0,
        BOOT_HANDOFF_DISABLE_BANK1,
        BOOT_HANDOFF_QUIESCE_SOURCES,
        BOOT_HANDOFF_CLEAR_CORE_PENDING,
        BOOT_HANDOFF_CLEAR_BANK0_PENDING,
        BOOT_HANDOFF_CLEAR_BANK1_PENDING,
        BOOT_HANDOFF_BARRIER_BEFORE_VTOR,
        BOOT_HANDOFF_SET_VTOR,
        BOOT_HANDOFF_BARRIER_AFTER_VTOR
    };
    /* Literal approved trace, independent of enum ordinal values. */
    output_unchanged(m);
    assert(m->calls < sizeof expected / sizeof expected[0]);
    assert(op == expected[m->calls]);
    ++m->calls;
    assert(value == (op == BOOT_HANDOFF_SET_VTOR ?
        (m->r.selected_slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE) : 0u) ||
        m->mutate != 0u);
    nested(m);
    if (m->mutate != 0u) memset(&m->r, 0xa5, sizeof m->r);
    output_unchanged(m);
    if (m->calls == m->fail && m->after == 0u) return -1;
    ++m->effects;
    switch (op)
    {
        case BOOT_HANDOFF_EXCLUDE: m->excluded = 1u; break;
        case BOOT_HANDOFF_STOP_SYSTICK: assert(m->excluded); m->stopped = 1u; break;
        case BOOT_HANDOFF_DISABLE_BANK0: assert(m->stopped); m->disabled[0] = 1u; break;
        case BOOT_HANDOFF_DISABLE_BANK1: assert(m->disabled[0]); m->disabled[1] = 1u; break;
        case BOOT_HANDOFF_QUIESCE_SOURCES:
            assert(m->disabled[0] && m->disabled[1]); m->quiescent = 1u; break;
        case BOOT_HANDOFF_CLEAR_CORE_PENDING:
            assert(m->quiescent && m->stopped); m->core_pending = 0u; break;
        case BOOT_HANDOFF_CLEAR_BANK0_PENDING:
            assert(m->quiescent); m->pending[0] = 0u; break;
        case BOOT_HANDOFF_CLEAR_BANK1_PENDING:
            assert(m->quiescent); m->pending[1] = 0u; break;
        case BOOT_HANDOFF_BARRIER_BEFORE_VTOR:
            assert(!m->pending[0] && !m->pending[1] && !m->core_pending); break;
        case BOOT_HANDOFF_SET_VTOR: m->vtor = value; break;
        case BOOT_HANDOFF_BARRIER_AFTER_VTOR: assert(m->vtor != 0u); break;
        default: assert(0); break;
    }
    /* Live peripheral work immediately repends even with delivery disabled. */
    if (m->quiescent == 0u) m->pending[0] = m->pending[1] = m->core_pending = 1u;
    output_unchanged(m);
    return m->calls == m->fail ? -1 : 0;
}

static int terminal(void *context, const boot_handoff_intent_t *intent)
{
    model_t *m = context;
    output_unchanged(m);
    ++m->calls;
    assert(m->calls == 12u && m->effects == 11u);
    assert(m->excluded && m->stopped && m->quiescent);
    assert(!m->pending[0] && !m->pending[1] && !m->core_pending);
    assert(m->w.owner == BOOT_WATCHDOG_APPLICATION && m->w.epoch == intent->application_epoch);
    assert(m->w.evidence == 0u && m->w.configured && !m->w.blocked);
    nested(m);
    if (m->mutate != 0u) memset(&m->r, 0x5a, sizeof m->r);
    output_unchanged(m);
    if (m->calls == m->fail && m->after == 0u) return -1;
    ++m->effects;
    m->recorded = *intent;
    m->terminal = 1u;
    output_unchanged(m);
    return m->calls == m->fail ? -1 : 0;
}

static void setup(model_t *m, uint32_t slot)
{
    boot_watchdog_io_t watchdog_io;
    boot_handoff_io_t io;
    memset(m, 0, sizeof *m);
    m->r = request(slot);
    m->lock = 1u;
    m->pending[0] = m->pending[1] = m->core_pending = 1u;
    watchdog_io.context = m;
    watchdog_io.read_register = read_register;
    watchdog_io.write_register = write_register;
    assert(boot_watchdog_init(&m->w, &watchdog_io) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_configure(&m->w, 100u, 500u) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&m->w, 1u, 1u, BOOT_VERIFY_MANIFEST_BEGIN,
        0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&m->w, 1u, 1u, BOOT_VERIFY_MANIFEST_AUTHENTICATED,
        0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
    io.context = m; io.operation = operation; io.terminal_intent = terminal;
    assert(boot_handoff_init(&m->h, &io) == BOOT_HANDOFF_OK);
}

static void trace(void)
{
    uint32_t slot;
    for (slot = 0u; slot <= 1u; ++slot)
    {
        unsigned mutate;
        for (mutate = 0u; mutate <= 1u; ++mutate)
        {
            model_t m;
            boot_handoff_intent_t out, expected;
            unsigned io_count;
            setup(&m, slot);
            assert(boot_handoff_admit(&m.r, &expected) == BOOT_HANDOFF_OK);
            m.reenter = 1u; m.mutate = mutate;
            io_count = m.watchdog_io;
            observe_output(&m, &out);
            assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_OK);
            assert(memcmp(&out, &expected, sizeof out) == 0);
            assert(memcmp(&m.recorded, &expected, sizeof out) == 0);
            assert(m.calls == 12u && m.effects == 12u && m.terminal && m.excluded);
            assert(m.watchdog_io == io_count && m.load == 500u && m.ctl == 3u && m.lock == 1u);
            assert(m.w.epoch == 2u && m.w.owner == BOOT_WATCHDOG_APPLICATION);
            assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_STATE);
            assert(m.calls == 12u && m.watchdog_io == io_count);
        }
    }
}

static void successful_retry(model_t *m, boot_handoff_intent_t *out)
{
    boot_handoff_intent_t expected;
    unsigned count = m->watchdog_io;
    assert(boot_handoff_admit(&m->r, &expected) == BOOT_HANDOFF_OK);
    output_unchanged(m);
    assert(boot_handoff_run(&m->h, &m->w, &m->r, out) == BOOT_HANDOFF_OK);
    assert(memcmp(out, &expected, sizeof *out) == 0);
    assert(memcmp(&m->recorded, &expected, sizeof expected) == 0);
    assert(m->calls == 12u && m->effects == 12u && m->terminal);
    assert(m->h.finished && !m->h.blocked);
    assert(m->w.owner == BOOT_WATCHDOG_APPLICATION && m->w.epoch == 2u);
    assert(m->w.evidence == 0u && m->watchdog_io == count);
}

static void run_rejected(const boot_handoff_request_t *r)
{
    model_t m;
    boot_handoff_intent_t out, before;
    unsigned count;
    setup(&m, BOOT_SLOT_A);
    m.r = *r;
    count = m.watchdog_io;
    observe_output(&m, &out); before = out;
    assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_ARGUMENT);
    assert(memcmp(&out, &before, sizeof out) == 0);
    assert(m.calls == 0u && m.watchdog_io == count);
    assert(!m.h.blocked && !m.h.finished && !m.excluded);
    assert(m.w.owner == BOOT_WATCHDOG_BOOT && m.w.epoch == 1u && m.w.evidence == 1u);
    /* Correct only the rejected request; reuse both initialized objects. */
    m.r = request(BOOT_SLOT_A);
    successful_retry(&m, &out);
}

static void last_epoch(void)
{
    uint32_t slot;
    for (slot = 0u; slot <= 1u; ++slot)
    {
        model_t m;
        boot_handoff_intent_t out;
        unsigned count;
        setup(&m, slot);
        m.w.epoch = m.r.watchdog_epoch = UINT32_MAX - 1u;
        m.r.observed.control = 2u;
        m.r.observed.primask = m.r.observed.faultmask = 1u;
        m.r.observed.basepri = 224u;
        m.r.image.manifest.payload_length = BOOT_PAYLOAD_MAX;
        m.r.image.reset_vector = (m.r.image.manifest.execution_base + BOOT_PAYLOAD_MAX - 2u) | 1u;
        count = m.watchdog_io;
        observe_output(&m, &out);
        assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_OK);
        assert(out.application_epoch == UINT32_MAX && m.w.epoch == UINT32_MAX);
        assert(out.vtor == m.r.image.manifest.execution_base && out.msp == BOOT_INITIAL_MSP);
        assert(out.reset_vector == m.r.image.reset_vector);
        assert(!out.control && !out.primask && !out.basepri && !out.faultmask);
        assert(boot_watchdog_transfer(&m.w, UINT32_MAX - 1u) == BOOT_WATCHDOG_STATE);
        assert(boot_watchdog_service(&m.w, BOOT_WATCHDOG_BOOT, UINT32_MAX - 1u) == BOOT_WATCHDOG_STATE);
        assert(m.watchdog_io == count);
    }
}

static void watchdog_states(void)
{
    unsigned kind;
    for (kind = 0u; kind < 7u; ++kind)
    {
        model_t m;
        boot_handoff_intent_t out, before;
        unsigned count;
        setup(&m, BOOT_SLOT_A);
        switch (kind)
        {
            case 0u: m.w.configured = 0u; break;
            case 1u: m.w.blocked = 1u; break;
            case 2u: m.w.owner = BOOT_WATCHDOG_APPLICATION; break;
            case 3u: m.w.epoch = 2u; break;
            case 4u: m.w.epoch = 0u; break;
            case 5u: m.w.epoch = UINT32_MAX; break;
            case 6u:
                assert(!atomic_flag_test_and_set_explicit(&m.w.entered, memory_order_acquire));
                break;
            default: assert(0); break;
        }
        count = m.watchdog_io;
        observe_output(&m, &out); before = out;
        assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) ==
            (kind == 6u ? BOOT_HANDOFF_BUSY : BOOT_HANDOFF_STATE));
        assert(memcmp(&out, &before, sizeof out) == 0);
        assert(m.calls == 0u && m.watchdog_io == count && !m.h.blocked && !m.excluded);
        assert(m.w.evidence == 1u);
        if (kind == 6u)
        {
            assert(atomic_flag_test_and_set_explicit(&m.w.entered, memory_order_acquire));
            atomic_flag_clear_explicit(&m.w.entered, memory_order_release);
        }
        /* Restore the preflight condition, never reinitialize either object. */
        m.w.configured = 1u;
        m.w.blocked = 0u;
        m.w.owner = BOOT_WATCHDOG_BOOT;
        m.w.epoch = 1u;
        successful_retry(&m, &out);
    }
}

static void arguments(void)
{
    static const unsigned run_null[] = {0u, 1u, 2u, 3u};
    static const unsigned admit_null[] = {0u, 1u};
    static const unsigned init_invalid[] = {0u, 1u, 2u, 3u};
    size_t i;
    for (i = 0u; i < sizeof run_null / sizeof run_null[0]; ++i)
    {
        model_t m;
        boot_handoff_intent_t out;
        unsigned count;
        setup(&m, BOOT_SLOT_A);
        observe_output(&m, &out);
        count = m.watchdog_io;
        assert(boot_handoff_run(run_null[i] == 0u ? NULL : &m.h,
            run_null[i] == 1u ? NULL : &m.w,
            run_null[i] == 2u ? NULL : &m.r,
            run_null[i] == 3u ? NULL : &out) == BOOT_HANDOFF_ARGUMENT);
        output_unchanged(&m);
        assert(m.calls == 0u && m.watchdog_io == count);
        assert(!m.h.blocked && !m.h.finished && !m.excluded);
        assert(m.w.owner == BOOT_WATCHDOG_BOOT && m.w.epoch == 1u && m.w.evidence == 1u);
        successful_retry(&m, &out);
    }
    for (i = 0u; i < sizeof admit_null / sizeof admit_null[0]; ++i)
    {
        boot_handoff_request_t r = request(BOOT_SLOT_A);
        boot_handoff_intent_t out, before;
        memset(&out, 0xa5, sizeof out); before = out;
        assert(boot_handoff_admit(admit_null[i] == 0u ? NULL : &r,
            admit_null[i] == 1u ? NULL : &out) == BOOT_HANDOFF_ARGUMENT);
        assert(memcmp(&out, &before, sizeof out) == 0);
        assert(boot_handoff_admit(&r, &out) == BOOT_HANDOFF_OK);
        assert(out.vtor == BOOT_SLOT_A_BASE && out.application_epoch == 2u);
    }
    for (i = 0u; i < sizeof init_invalid / sizeof init_invalid[0]; ++i)
    {
        boot_handoff_t fresh;
        boot_handoff_io_t io = {NULL, operation, terminal};
        unsigned char before[sizeof fresh];
        /* Failed init must leave fresh, unpublished storage untouched. */
        memset(&fresh, 0xa5, sizeof fresh);
        memcpy(before, &fresh, sizeof fresh);
        if (init_invalid[i] == 2u) io.operation = NULL;
        if (init_invalid[i] == 3u) io.terminal_intent = NULL;
        assert(boot_handoff_init(init_invalid[i] == 0u ? NULL : &fresh,
            init_invalid[i] == 1u ? NULL : &io) == BOOT_HANDOFF_ARGUMENT);
        assert(memcmp(&fresh, before, sizeof fresh) == 0);
        io.operation = operation;
        io.terminal_intent = terminal;
        /* This is the first successful init, not a forbidden reinit. */
        assert(boot_handoff_init(&fresh, &io) == BOOT_HANDOFF_OK);
        assert(fresh.io.context == NULL && fresh.io.operation == operation &&
            fresh.io.terminal_intent == terminal);
        assert(!fresh.blocked && !fresh.finished);
        assert(!atomic_flag_test_and_set_explicit(&fresh.entered, memory_order_acquire));
        atomic_flag_clear_explicit(&fresh.entered, memory_order_release);
    }
}

static void faults(void)
{
    uint32_t slot;
    for (slot = 0u; slot <= 1u; ++slot)
    {
        unsigned fail, after;
        for (fail = 1u; fail <= 12u; ++fail)
        for (after = 0u; after <= 1u; ++after)
        {
            model_t m;
            boot_handoff_intent_t out, before;
            unsigned count;
            setup(&m, slot);
            m.fail = fail; m.after = after; m.reenter = 1u;
            count = m.watchdog_io;
            observe_output(&m, &out); before = out;
            assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_IO);
            assert(memcmp(&out, &before, sizeof out) == 0);
            assert(m.h.blocked && !m.h.finished && m.calls == fail);
            assert(m.effects == fail - 1u + after);
            assert(m.excluded == (fail > 1u || after != 0u));
            assert(m.watchdog_io == count && m.load == 500u && m.ctl == 3u && m.lock == 1u);
            assert(m.w.configured && !m.w.blocked);
            assert(m.w.owner == (fail == 12u ? BOOT_WATCHDOG_APPLICATION : BOOT_WATCHDOG_BOOT));
            assert(m.w.epoch == (fail == 12u ? 2u : 1u));
            assert(m.w.evidence == (fail == 12u ? 0u : 1u));
            assert(m.terminal == (fail == 12u && after != 0u));
            assert(boot_handoff_run(&m.h, &m.w, &m.r, &out) == BOOT_HANDOFF_BLOCKED);
            assert(m.calls == fail && m.watchdog_io == count);
            assert(memcmp(&out, &before, sizeof out) == 0);
        }
    }
}

int main(void)
{
    admission();
    arguments();
    trace();
    watchdog_states();
    last_epoch();
    faults();
    puts("handoff admission + arguments + corrected retries + trace + 48 before/after faults PASS");
    return 0;
}
