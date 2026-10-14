#include "lm3s6965/boot_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    boot_diag_publication_t owner;
    uint32_t words[16];
    uint8_t pages[2][BOOT_METADATA_BYTES];
    boot_verified_image_t running;
    unsigned reads, runs;
    bool read_fail, run_fail, diag_fail;
} fixture_t;
static void busy(fixture_t *f)
{
    boot_diag_t d; boot_diag_context_t c = {0};
    assert(f->owner.busy);
    assert(boot_diag_publication_boot(&f->owner, 0) == BOOT_DIAG_PUBLICATION_BUSY);
    assert(boot_diag_publication_progress(&f->owner, 0, 0) == BOOT_DIAG_PUBLICATION_BUSY);
    assert(boot_diag_publication_adopt(&f->owner, &c, true) == BOOT_DIAG_PUBLICATION_BUSY);
    assert(boot_diag_publication_snapshot(&f->owner, &d, &c) == BOOT_DIAG_PUBLICATION_BUSY);
}
static int read_metadata(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    unsigned page = address == BOOT_METADATA_PAGE0 ? 0u : 1u;
    busy(f); ++f->reads;
    assert(address == BOOT_METADATA_PAGE0 || address == BOOT_METADATA_PAGE1);
    assert(size == BOOT_METADATA_BYTES);
    if (f->read_fail) return -1;
    memcpy(bytes, f->pages[page], size);
    return 0;
}
static int running(void *context, boot_verified_image_t *out)
{
    fixture_t *f = context;
    busy(f); ++f->runs;
    if (f->run_fail) return -1;
    *out = f->running;
    return 0;
}
static bool diag_read(void *context, unsigned index, bool write, uint32_t *value)
{
    fixture_t *f = context;
    assert(index < 16 && !write && f->owner.busy);
    if (f->diag_fail) return false;
    *value = f->words[index];
    return true;
}
static boot_metadata_t metadata(void)
{
    boot_metadata_t m = {0};
    m.sequence = 10; m.current_slot = BOOT_SLOT_A;
    m.candidate_slot = m.previous_slot = m.intent_slot = BOOT_SLOT_NONE;
    m.confirmed_mask = BOOT_CONFIRMED_A;
    m.images[0].payload_length = BOOT_PAYLOAD_MIN;
    m.images[0].payload_hash[0] = 42;
    return m;
}
static void install(fixture_t *f, unsigned page, const boot_metadata_t *m)
{
    assert(boot_metadata_encode(m, BOOT_COMMIT_VALID, f->pages[page],
        sizeof f->pages[page]) == BOOT_OK);
}
static void init(fixture_t *f)
{
    boot_metadata_t m = metadata();
    boot_diag_t d; boot_diag_context_t c;
    uint8_t wire[BOOT_DIAG_BYTES]; unsigned i;
    memset(f, 0, sizeof(*f)); memset(f->pages, 0xff, sizeof f->pages);
    f->owner.words = f->words;
    assert(boot_diag_init(123, &d, &c) == BOOT_DIAG_OK);
    d.fault = BOOT_DIAG_FAULT_VALIDATION; d.detail = BOOT_DIAG_DETAIL_HASH;
    d.failed_slot = BOOT_SLOT_B; d.selected_slot = BOOT_SLOT_B;
    d.running_slot = BOOT_SLOT_B; d.sequence = UINT32_MAX;
    d.trial_id = UINT32_MAX; d.failures = 3;
    c.sequence_known = c.trial_id_known = c.failures_known = true;
    assert(boot_diag_encode(&d, BOOT_DIAG_PUBLISHED, wire, sizeof wire) == BOOT_DIAG_OK);
    for (i = 0; i < 16; ++i) f->words[i] = (uint32_t)wire[4*i] |
        ((uint32_t)wire[4*i+1] << 8) | ((uint32_t)wire[4*i+2] << 16) |
        ((uint32_t)wire[4*i+3] << 24);
    assert(boot_diag_publication_adopt(&f->owner, &c, true) == BOOT_DIAG_PUBLICATION_OK);
    install(f, 0, &m);
    f->running.manifest.slot = BOOT_SLOT_A;
    f->running.manifest.execution_base = BOOT_SLOT_A_BASE;
    f->running.manifest.payload_length = BOOT_PAYLOAD_MIN;
    f->running.manifest.payload_hash[0] = 42;
    f->running.initial_msp = BOOT_INITIAL_MSP;
    f->running.reset_vector = BOOT_SLOT_A_BASE | 1u;
}
static void expect(fixture_t *f, boot_snapshot_result_t result)
{
    boot_snapshot_sources_t s = {{f, read_metadata, NULL, NULL}, f, running};
    boot_snapshot_t out, before;
    uint32_t words[16];
    memset(&out, 0xa5, sizeof out); before = out;
    memcpy(words, f->words, sizeof words);
    boot_snapshot_result_t actual = boot_snapshot_capture(&f->owner, &s, &out);
    if (actual != result) fprintf(stderr, "expected %u got %u (sequence %u)\n", (unsigned)result,
        (unsigned)actual, out.metadata.record.sequence);
    assert(actual == result);
    if (result != BOOT_SNAPSHOT_OK) assert(memcmp(&out, &before, sizeof out) == 0);
    assert(memcmp(words, f->words, sizeof words) == 0 && !f->owner.busy);
}
int main(void)
{
    fixture_t f; boot_metadata_t m; boot_snapshot_t out, frozen;
    boot_snapshot_sources_t s = {{&f, read_metadata, NULL, NULL}, &f, running};
    init(&f); expect(&f, BOOT_SNAPSHOT_OK);
    assert(f.reads == 2 && f.runs == 1);
    assert(boot_snapshot_capture(&f.owner, &s, &out) == BOOT_SNAPSHOT_OK);
    assert(out.diagnostic.sequence == UINT32_MAX && out.diagnostic.trial_id == UINT32_MAX &&
        out.diagnostic.failures == 3 && out.diagnostic.selected_slot == BOOT_SLOT_B &&
        out.diagnostic_context.sequence_known && out.diagnostic_context.trial_id_known &&
        out.diagnostic_context.failures_known && out.metadata.record.sequence == 10 &&
        out.running.manifest.slot == BOOT_SLOT_A);
    frozen = out;
    assert(boot_diag_publication_progress(&f.owner, 4, 7) == BOOT_DIAG_PUBLICATION_OK);
    m = metadata(); m.sequence = 11; install(&f, 0, &m);
    f.running.manifest.release = 99;
    assert(memcmp(&out, &frozen, sizeof out) == 0);
    expect(&f, BOOT_SNAPSHOT_IDENTITY); /* stale evidence with changed identity fails */
    init(&f); f.read_fail = true; expect(&f, BOOT_SNAPSHOT_METADATA); assert(f.runs == 0);
    init(&f); m = metadata(); m.images[0].release = 1; install(&f, 1, &m);
    expect(&f, BOOT_SNAPSHOT_METADATA); /* equal-sequence conflict */
    init(&f); memset(f.pages, 0xff, sizeof f.pages); expect(&f, BOOT_SNAPSHOT_METADATA);
    init(&f); f.owner.word = diag_read; f.owner.user = &f; f.diag_fail = true;
    expect(&f, BOOT_SNAPSHOT_PUBLICATION); assert(f.reads == 0);
    init(&f); f.run_fail = true; expect(&f, BOOT_SNAPSHOT_RUNNING);
    init(&f); f.running.manifest.payload_hash[1] = 1; expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); ++f.running.manifest.payload_length; expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); ++f.running.manifest.execution_base; expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); f.running.initial_msp = 0; expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); f.running.reset_vector &= ~1u; expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); f.running.reset_vector = BOOT_SLOT_A_BASE + BOOT_PAYLOAD_MIN + 1u;
    expect(&f, BOOT_SNAPSHOT_IDENTITY);
    init(&f); f.words[0] = 0; expect(&f, BOOT_SNAPSHOT_PUBLICATION); assert(f.reads == 0);
    init(&f); f.words[3] = 0; expect(&f, BOOT_SNAPSHOT_PUBLICATION);
    init(&f); f.owner.associated = false; expect(&f, BOOT_SNAPSHOT_PUBLICATION);
    /* A currently executing trial candidate is not required to be current. */
    init(&f); m = metadata(); m.sequence = 20; m.candidate_slot = BOOT_SLOT_B;
    m.images[1] = m.images[0]; m.images[1].accounting = BOOT_STATE_TRIAL;
    m.images[1].trial_id = 7; install(&f, 0, &m);
    f.running.manifest.slot = BOOT_SLOT_B; f.running.manifest.execution_base = BOOT_SLOT_B_BASE;
    f.running.reset_vector = BOOT_SLOT_B_BASE | 1u;
    expect(&f, BOOT_SNAPSHOT_OK);
    /* Role evolution does not rewrite/reject historical fault evidence. */
    m.sequence++; m.current_slot = BOOT_SLOT_B; m.candidate_slot = BOOT_SLOT_NONE;
    m.previous_slot = BOOT_SLOT_A; m.confirmed_mask |= BOOT_CONFIRMED_B;
    m.images[1].accounting = BOOT_STATE_NONE; install(&f, 0, &m);
    expect(&f, BOOT_SNAPSHOT_OK);
    f.running.manifest.slot = BOOT_SLOT_A; f.running.manifest.execution_base = BOOT_SLOT_A_BASE;
    f.running.reset_vector = BOOT_SLOT_A_BASE | 1u;
    expect(&f, BOOT_SNAPSHOT_OK); /* actually running previous confirmed */
    f.owner.context.sequence_known = false; f.owner.context.trial_id_known = false;
    assert(boot_snapshot_capture(&f.owner, &s, &out) == BOOT_SNAPSHOT_OK);
    assert(out.diagnostic.sequence == UINT32_MAX && !out.diagnostic_context.sequence_known &&
        out.diagnostic.trial_id == UINT32_MAX && !out.diagnostic_context.trial_id_known);
    /* Numeric zero has no sentinel meaning either; availability is preserved. */
    init(&f); f.words[10] = 0; f.words[11] = 0;
    assert(boot_snapshot_capture(&f.owner, &s, &out) == BOOT_SNAPSHOT_OK);
    assert(out.diagnostic.sequence == 0 && out.diagnostic.trial_id == 0 &&
        out.diagnostic_context.sequence_known && out.diagnostic_context.trial_id_known);
    assert(boot_snapshot_capture(NULL, &s, &out) == BOOT_SNAPSHOT_ARGUMENT);
    assert(boot_snapshot_capture(&f.owner, NULL, &out) == BOOT_SNAPSHOT_ARGUMENT);
    assert(boot_snapshot_capture(&f.owner, &s, NULL) == BOOT_SNAPSHOT_ARGUMENT);
    /* A replay with a different metadata identity fails above. A stale/lying
     * source returning the same valid identity or racing outside this owner
     * cannot be detected by sampling: that violates the source contract, not
     * a reason to infer a diagnostic source or add an event-generation gate. */
    puts("snapshot tests passed");
    return 0;
}
