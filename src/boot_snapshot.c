#include "lm3s6965/boot_snapshot.h"

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}
typedef struct {
    const boot_snapshot_sources_t *sources;
    boot_snapshot_t value;
    boot_snapshot_result_t result;
} capture_t;

/* boot_store_load requires a complete adapter, though it only calls read.
 * Supply fail-closed write stubs; never expose caller write capabilities. */
static int no_erase(void *context, uint32_t address)
{
    (void)context; (void)address; return -1;
}
static int no_program(void *context, uint32_t address, uint32_t value)
{
    (void)context; (void)address; (void)value; return -1;
}
static bool capture(void *context, const boot_diag_t *diag,
    const boot_diag_context_t *availability)
{
    capture_t *c = context;
    boot_verified_image_t *r = &c->value.running;
    uint32_t end, reset;
    boot_store_adapter_t reader = {c->sources->metadata.context,
        c->sources->metadata.read, no_erase, no_program};
    c->value.diagnostic = *diag;
    c->value.diagnostic_context = *availability;
    c->result = BOOT_SNAPSHOT_METADATA;
    if (boot_store_load(&reader, &c->value.metadata) != BOOT_STORE_OK)
        return false;
    c->result = BOOT_SNAPSHOT_RUNNING;
    if (c->sources->running(c->sources->running_context, r) != 0) return false;
    c->result = BOOT_SNAPSHOT_IDENTITY;
    /* Structural identity and vectors only; authenticity is the source's
     * explicit contract, not something inferable from a decoded structure. */
    if (boot_metadata_image_matches(&c->value.metadata.record, &r->manifest) != BOOT_OK ||
        boot_image_span(r->manifest.slot, r->manifest.execution_base,
            r->manifest.payload_length, &end) != BOOT_OK) return false;
    reset = r->reset_vector & ~UINT32_C(1);
    if (r->initial_msp != BOOT_INITIAL_MSP || (r->reset_vector & 1u) == 0u ||
        reset < r->manifest.execution_base || reset >= end) return false;
    c->result = BOOT_SNAPSHOT_OK;
    return true;
}
boot_snapshot_result_t boot_snapshot_capture(boot_diag_publication_t *owner,
    const boot_snapshot_sources_t *sources, boot_snapshot_t *out)
{
    capture_t c;
    boot_diag_publication_result_t result;
    if (owner == NULL || sources == NULL || out == NULL ||
        sources->metadata.read == NULL || sources->running == NULL ||
        overlaps(out, sizeof(*out), sources, sizeof(*sources)) ||
        overlaps(out, sizeof(*out), owner, sizeof(*owner)) ||
        overlaps(sources, sizeof(*sources), owner, sizeof(*owner)) ||
        overlaps(out, sizeof(*out), (const void *)owner->words, BOOT_DIAG_BYTES) ||
        overlaps(sources, sizeof(*sources), (const void *)owner->words, BOOT_DIAG_BYTES))
        return BOOT_SNAPSHOT_ARGUMENT;
    c.sources = sources;
    c.result = BOOT_SNAPSHOT_PUBLICATION;
    result = boot_diag_publication_capture(owner, capture, &c);
    if (result != BOOT_DIAG_PUBLICATION_OK)
        return result == BOOT_DIAG_PUBLICATION_CAPTURE ? c.result : BOOT_SNAPSHOT_PUBLICATION;
    *out = c.value;
    return BOOT_SNAPSHOT_OK;
}
