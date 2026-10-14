#include "flash_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static boot_metadata_t fixture(void)
{
    boot_metadata_t r = {0};
    r.sequence = 10u; r.current_slot = BOOT_SLOT_A;
    r.candidate_slot = r.previous_slot = r.intent_slot = BOOT_SLOT_NONE;
    r.confirmed_mask = BOOT_CONFIRMED_A;
    r.images[0].payload_length = BOOT_PAYLOAD_MIN;
    r.images[0].payload_hash[0] = 42u;
    return r;
}
static void install(flash_model_t *m, unsigned page, const boot_metadata_t *r)
{
    CHECK(boot_metadata_encode(r, BOOT_COMMIT_VALID,
          m->bytes + page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
}
static void reboot(flash_model_t *m, const uint8_t *old, const uint8_t *next)
{
    boot_metadata_t r;
    unsigned page;
    boot_result_t result = boot_metadata_select(m->bytes, BOOT_ERASE_BYTES,
        m->bytes + BOOT_ERASE_BYTES, BOOT_ERASE_BYTES, &r, &page);
    /* Under this bounded fault model the untouched old page must remain
     * valid: recovery is allowed by the API, but not needed for these cuts. */
    CHECK(result == BOOT_OK);
    if (result == BOOT_OK)
        CHECK(memcmp(m->bytes + page * BOOT_ERASE_BYTES, old, BOOT_METADATA_BYTES) == 0 ||
              memcmp(m->bytes + page * BOOT_ERASE_BYTES, next, BOOT_METADATA_BYTES) == 0);
}
static void cuts(unsigned source)
{
    flash_model_t base, m;
    boot_store_adapter_t a;
    boot_store_snapshot_t expected, out, sentinel;
    boot_metadata_t r = fixture(), desired;
    uint8_t old[BOOT_METADATA_BYTES], next[BOOT_METADATA_BYTES];
    size_t calls, call, bits;
    unsigned mode;
    flash_model_init(&base); install(&base, source, &r);
    /* Dirty destination, including tail: erase must wipe it all. */
    memset(base.bytes + (1u-source)*BOOT_ERASE_BYTES, 0, BOOT_ERASE_BYTES);
    a = flash_model_adapter(&base);
    CHECK(boot_store_load(&a, &expected) == BOOT_STORE_OK);
    desired = r; ++desired.sequence; desired.images[0].release = 123u;
    memcpy(old, base.bytes + source * BOOT_ERASE_BYTES, sizeof old);
    CHECK(boot_metadata_encode(&desired, BOOT_COMMIT_VALID, next, sizeof next) == BOOT_OK);
    base.calls = 0;
    m = base; a = flash_model_adapter(&m);
    CHECK(boot_store_replace(&a, &expected, &desired, &out) == BOOT_STORE_OK);
    calls = m.calls;
    CHECK(calls == 50u); /* read2 erase read erase-check write41 stage commit final read2 */
    CHECK(out.page == 1u-source && out.record.sequence == desired.sequence);
    CHECK(memcmp(m.bytes + source*BOOT_ERASE_BYTES, base.bytes + source*BOOT_ERASE_BYTES, BOOT_ERASE_BYTES) == 0);
    for (bits = BOOT_METADATA_BYTES; bits < BOOT_ERASE_BYTES; ++bits)
        CHECK(m.bytes[(1u-source)*BOOT_ERASE_BYTES + bits] == UINT8_MAX);
    memset(&sentinel, 0xa5, sizeof sentinel);
    /* Every callback boundary; every prefix bit cut in erase and each word.
     * Full physical write followed by callback error is separately covered. */
    for (call = 1; call <= calls; ++call)
    {
        size_t max = call == 3u ? BOOT_ERASE_BYTES*8u :
                     ((call >= 5u && call <= 45u) || call == 47u ? 32u : 0u);
        for (bits = 0; bits <= max; ++bits)
        {
            boot_store_result_t result;
            m = base; m.cut_call = call; m.cut_bits = bits;
            a = flash_model_adapter(&m); out = sentinel;
            result = boot_store_replace(&a, &expected, &desired, &out);
            CHECK(result != BOOT_STORE_OK);
            CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
            CHECK(result == (call >= 47u ? BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
            CHECK(memcmp(m.bytes + source*BOOT_ERASE_BYTES, base.bytes + source*BOOT_ERASE_BYTES, BOOT_ERASE_BYTES) == 0);
            reboot(&m, old, next);
            {
                boot_metadata_t selected;
                unsigned page;
                CHECK(boot_metadata_select(m.bytes, BOOT_ERASE_BYTES,
                    m.bytes + BOOT_ERASE_BYTES, BOOT_ERASE_BYTES, &selected, &page) == BOOT_OK);
                CHECK(selected.sequence == ((call > 47u || (call == 47u && bits == 32u))
                      ? desired.sequence : expected.record.sequence));
            }
        }
        for (mode = 0; mode < 3u; ++mode)
        {
            boot_store_result_t result;
            m = base; m.cut_call = call; m.fail_after = 1;
            m.corrupt_read = mode == 1u; m.silent_program = mode == 2u;
            a = flash_model_adapter(&m); out = sentinel;
            result = boot_store_replace(&a, &expected, &desired, &out);
            /* Skipping an all-ones word or corrupting the already-invalid
             * destination's initial read can leave the result unaffected. */
            if (result == BOOT_STORE_OK)
            {
                CHECK(mode != 0u);
                CHECK(out.record.sequence == desired.sequence);
                CHECK(memcmp(m.bytes + out.page*BOOT_ERASE_BYTES, next, sizeof next) == 0);
            }
            else CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
            reboot(&m, old, next);
        }
    }
}
static void admission(void)
{
    flash_model_t m;
    boot_store_adapter_t a;
    boot_store_snapshot_t s, out, sentinel;
    boot_metadata_t r = fixture(), desired;
    flash_model_init(&m); a = flash_model_adapter(&m);
    memset(&sentinel, 0x5a, sizeof sentinel); out = sentinel;
    CHECK(boot_store_load(&a, &out) == BOOT_STORE_NO_RECORD);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    s.record = r; s.page = 0u; desired = r; ++desired.sequence;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_NO_RECORD);
    CHECK(m.erases == 0u && m.writes == 0u);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    install(&m, 0u, &r); install(&m, 1u, &r);
    CHECK(boot_store_load(&a, &s) == BOOT_STORE_OK && s.page == 0u);
    desired = r; ++desired.sequence;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_OK);
    CHECK(m.erases == 1u && m.writes == 42u);
    m.erases = m.writes = 0;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_STALE);
    CHECK(m.erases == 0u && m.writes == 0u);
    r.images[0].release = 1u; install(&m, 0u, &r);
    r.images[0].release = 2u; install(&m, 1u, &r);
    CHECK(boot_store_load(&a, &out) == BOOT_STORE_CONFLICT);
    s.record = fixture(); s.page = 0u; desired = s.record; ++desired.sequence;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_CONFLICT);
    flash_model_init(&m); install(&m, 0u, &s.record);
    desired = s.record;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_SEQUENCE);
    desired.sequence += 2u;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_SEQUENCE);
    desired.sequence = 11u; desired.confirmed_mask = 4u;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_INVALID_RECORD);
    desired = s.record; desired.sequence = 11u;
    s.record.images[0].release = 99u;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_STALE);
    s.record = fixture(); s.record.sequence = UINT32_MAX;
    install(&m, 0u, &s.record); desired = s.record;
    CHECK(boot_store_load(&a, &out) == BOOT_STORE_OK && out.record.sequence == UINT32_MAX);
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_EXHAUSTED);
    desired.sequence = 0;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_EXHAUSTED);
    CHECK(m.erases == 0u && m.writes == 0u);
    s.record.sequence = UINT32_MAX-1u; install(&m, 0u, &s.record);
    desired.sequence = UINT32_MAX;
    CHECK(boot_store_replace(&a, &s, &desired, &out) == BOOT_STORE_OK);
    CHECK(boot_store_replace(NULL, &s, &desired, &out) == BOOT_STORE_ARGUMENT);
}
int main(void)
{
    admission(); cuts(0u); cuts(1u);
    printf("boot store: %u checks passed\n", checks);
    return 0;
}
