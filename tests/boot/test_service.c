#include "lm3s6965/boot_service.h"
#include "flash_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    flash_model_t flash;
    uint32_t words[16];
    boot_diag_publication_t owner;
    boot_verified_image_t running;
    boot_snapshot_sources_t sources;
    boot_confirm_service_t service;
    bool reenter;
} fixture_t;
static int running(void *context, boot_verified_image_t *out)
{
    fixture_t *f = context;
    assert(f->owner.busy);
    if (f->reenter) {
        boot_confirm_request_t r = {0}; boot_store_snapshot_t s;
        assert(boot_confirm_apply(&f->service, &f->owner, &f->sources, &r, &s) == BOOT_CONFIRM_BUSY);
    }
    *out = f->running;
    return 0;
}
static void init(fixture_t *f, unsigned page, uint32_t slot)
{
    memset(f, 0, sizeof *f); flash_model_init(&f->flash);
    boot_metadata_t m = {0};
    m.sequence = 20; m.current_slot = slot ^ 1u; m.candidate_slot = slot;
    m.previous_slot = m.intent_slot = BOOT_SLOT_NONE;
    m.confirmed_mask = 1u << m.current_slot;
    for (unsigned i = 0; i < 2; ++i) {
        m.images[i].payload_length = BOOT_PAYLOAD_MIN;
        m.images[i].payload_hash[0] = (uint8_t)(42u+i);
    }
    m.images[slot].accounting = BOOT_STATE_TRIAL; m.images[slot].trial_id = 19;
    assert(boot_metadata_encode(&m, BOOT_COMMIT_VALID,
        f->flash.bytes + page*BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
    f->owner.words = f->words;
    assert(boot_diag_publication_boot(&f->owner, 0) == BOOT_DIAG_PUBLICATION_OK);
    assert(boot_diag_publication_progress(&f->owner, 4, 7) == BOOT_DIAG_PUBLICATION_OK);
    f->running.manifest.slot = slot;
    f->running.manifest.execution_base = slot ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE;
    f->running.manifest.payload_length = BOOT_PAYLOAD_MIN;
    f->running.manifest.payload_hash[0] = (uint8_t)(42u+slot);
    f->running.initial_msp = BOOT_INITIAL_MSP;
    f->running.reset_vector = f->running.manifest.execution_base | 1u;
    f->sources.metadata = flash_model_adapter(&f->flash);
    f->sources.running_context = f; f->sources.running = running;
}
static boot_confirm_request_t request(const fixture_t *f)
{
    boot_confirm_request_t r = {0}; r.slot = f->running.manifest.slot; r.trial_id = 19;
    memcpy(r.payload_hash, f->running.manifest.payload_hash, 32); return r;
}
static void parser(void)
{
    const char *body = "trial_id=0004294967295\nhash=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\nslot=B\nformat=1\n";
    size_t n = strlen(body);
    for (size_t split = 0; split <= n; ++split) {
        boot_confirm_parser_t p = {0}; boot_confirm_request_t r;
        assert(boot_confirm_feed(&p, (const uint8_t *)body, split));
        assert(boot_confirm_feed(&p, (const uint8_t *)body+split, n-split));
        assert(boot_confirm_finish(&p, &r));
        assert(r.slot == BOOT_SLOT_B && r.trial_id == UINT32_MAX && r.payload_hash[31] == 0xef);
        assert(!boot_confirm_finish(&p, &r)); assert(!boot_confirm_feed(&p, NULL, 0));
    }
    for (size_t cut = 0; cut < n; ++cut) {
        boot_confirm_parser_t p = {0}; boot_confirm_request_t r, before;
        memset(&r, 0xa5, sizeof r); before = r;
        assert(boot_confirm_feed(&p, (const uint8_t *)body, cut));
        assert(!boot_confirm_finish(&p, &r)); assert(!memcmp(&r, &before, sizeof r));
    }
    const char *bad[] = {"format=1\nformat=1\n", "format=1\r\n", "format=1", "foo=1\n", "trial_id=4294967296\n", "slot=a\n", "\n"};
    for (size_t i = 0; i < sizeof bad/sizeof bad[0]; ++i) {
        boot_confirm_parser_t p = {0}; boot_confirm_request_t r;
        (void)boot_confirm_feed(&p, (const uint8_t *)bad[i], strlen(bad[i]));
        assert(!boot_confirm_finish(&p, &r));
    }
    char mutated[129];
    for (unsigned change = 0; change < 5; ++change) {
        strcpy(mutated, body);
        if (change == 0) strcat(mutated, "format=1\n");
        if (change == 1) *strstr(mutated, "hash=") = 'x';
        if (change == 2) strstr(mutated, "hash=")[5] = 'A';
        if (change == 3) strstr(mutated, "slot=B")[5] = 'a';
        if (change == 4) memcpy(strstr(mutated, "0004294967295"), "0004294967296", 13);
        boot_confirm_parser_t q = {0}; boot_confirm_request_t r;
        assert(boot_confirm_feed(&q, (const uint8_t *)mutated, strlen(mutated)));
        assert(!boot_confirm_finish(&q, &r));
    }
    uint8_t oversized[129]; memset(oversized, '0', sizeof oversized);
    boot_confirm_parser_t p = {0}; assert(!boot_confirm_feed(&p, oversized, sizeof oversized));
    p = (boot_confirm_parser_t){0}; oversized[0] = 0; assert(!boot_confirm_feed(&p, oversized, 1));
}
static void parser_decimal_format(void)
{
    const char *hash = "2b00000000000000000000000000000000000000000000000000000000000000";
    const char *valid[] = {"01", "0001"};
    const char *invalid[] = {"0", "2", "4294967296", "0004294967296", "4294967297",
                             "+1", "-1", " 1", "1 ", "1x", ""};
    fixture_t f; init(&f, 0, BOOT_SLOT_B);
    flash_model_t before_flash = f.flash;
    for (unsigned order = 0; order < 2; ++order) {
        for (size_t i = 0; i < sizeof valid/sizeof valid[0]; ++i) {
            char body[129];
            int length = order == 0 ?
                snprintf(body, sizeof body, "format=%s\nslot=B\nhash=%s\ntrial_id=00019\n", valid[i], hash) :
                snprintf(body, sizeof body, "trial_id=00019\nhash=%s\nslot=B\nformat=%s\n", hash, valid[i]);
            assert(length > 0 && (size_t)length < sizeof body);
            size_t n = (size_t)length;
            for (size_t split = 0; split <= n; ++split) {
                boot_confirm_parser_t p = {0}; boot_confirm_request_t r;
                assert(boot_confirm_feed(&p, (const uint8_t *)body, split));
                assert(boot_confirm_feed(&p, (const uint8_t *)body+split, n-split));
                assert(boot_confirm_finish(&p, &r));
                boot_confirm_request_t expected = request(&f);
                assert(r.slot == expected.slot && r.trial_id == expected.trial_id);
                assert(!memcmp(r.payload_hash, expected.payload_hash, sizeof r.payload_hash));
            }
        }
        for (size_t i = 0; i < sizeof invalid/sizeof invalid[0]; ++i) {
            char body[129];
            int length = order == 0 ?
                snprintf(body, sizeof body, "format=%s\nslot=B\nhash=%s\ntrial_id=19\n", invalid[i], hash) :
                snprintf(body, sizeof body, "trial_id=19\nhash=%s\nslot=B\nformat=%s\n", hash, invalid[i]);
            assert(length > 0 && (size_t)length < sizeof body);
            size_t n = (size_t)length;
            for (size_t split = 0; split <= n; ++split) {
                boot_confirm_parser_t p = {0}; boot_confirm_request_t r, sentinel;
                memset(&r, 0xa5, sizeof r); sentinel = r;
                assert(boot_confirm_feed(&p, (const uint8_t *)body, split));
                assert(boot_confirm_feed(&p, (const uint8_t *)body+split, n-split));
                assert(!boot_confirm_finish(&p, &r));
                assert(!memcmp(&r, &sentinel, sizeof r));
                assert(!memcmp(&f.flash, &before_flash, sizeof f.flash));
                assert(!f.flash.erases && !f.flash.writes);
            }
        }
    }
}
static void status_render(void)
{
    fixture_t f; boot_snapshot_t s; char text[1025], frozen[1025]; size_t n = 999;
    init(&f, 0, 1);
    assert(boot_snapshot_capture(&f.owner, &f.sources, &s) == BOOT_SNAPSHOT_OK);
    size_t calls = f.flash.calls;
    assert(boot_status_render(&s, text, 1024, &n)); text[n] = 0;
    assert(n < 600 && f.flash.calls == calls);
    const char *expected =
        "format=1\nsequence=20\nrunning_slot=B\nhash=2b00000000000000000000000000000000000000000000000000000000000000\n"
        "release=0\nconfirmed_slot=A\ncandidate_slot=B\npermitted_slot=none\ntrial_id=19\ntrial_state=trial\n"
        "slot_a_failures=0\nslot_a_reasons=none\nslot_a_rollback=0\nslot_b_failures=0\nslot_b_reasons=none\nslot_b_rollback=0\n"
        "health=00000007\nstage=4\nfault=0\ndetail=0\nreset_flags=00000000\n";
    assert(!strcmp(text, expected));
    assert(strstr(text, "candidate_slot=B\npermitted_slot=none\ntrial_id=19\ntrial_state=trial\n"));
    assert(strstr(text, "health=00000007\nstage=4\nfault=0\ndetail=0\nreset_flags=00000000\n"));
    strcpy(frozen, text); size_t old = n;
    assert(!boot_status_render(&s, text, n-1, &n)); assert(n == old && !strcmp(text, frozen));
    memset(&f.running, 0, sizeof f.running);
    assert(boot_status_render(&s, text, 1024, &n)); text[n] = 0; assert(!strcmp(text, frozen));
    s.diagnostic_context.health_known = false; s.diagnostic_context.reset_flags_known = false;
    assert(boot_status_render(&s, text, 1024, &n)); text[n] = 0;
    assert(strstr(text, "health=none\n") && strstr(text, "reset_flags=none\n"));
    s.metadata.record.sequence = UINT32_MAX;
    assert(boot_status_render(&s, text, 1024, &n)); text[n] = 0; assert(strstr(text, "sequence=4294967295\n"));
    s.metadata.record.images[1].trial_id = UINT32_MAX;
    s.metadata.record.current_slot = BOOT_SLOT_B;
    s.metadata.record.candidate_slot = BOOT_SLOT_NONE;
    s.metadata.record.confirmed_mask = BOOT_CONFIRMED_B;
    s.metadata.record.images[1].accounting = BOOT_STATE_NONE;
    memset(&s.metadata.record.images[0], 0, sizeof s.metadata.record.images[0]);
    assert(boot_status_render(&s, text, 1024, &n)); text[n] = 0;
    assert(strstr(text, "permitted_slot=A\ntrial_id=4294967295\ntrial_state=none\n"));
    assert(strstr(text, "slot_a_failures=none\nslot_a_reasons=none\nslot_a_rollback=none\n"));
}
static void durable(void)
{
    for (unsigned page = 0; page < 2; ++page) for (uint32_t slot = 0; slot < 2; ++slot) {
        fixture_t f; boot_store_snapshot_t out, sentinel; init(&f, page, slot);
        boot_confirm_request_t r = request(&f); f.reenter = true;
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_OK);
        size_t total = f.flash.calls;
        assert(total == 52);
        assert(out.record.sequence == 21 && out.record.current_slot == slot && out.record.candidate_slot == BOOT_SLOT_NONE);
        assert(f.flash.erases == 1 && f.flash.writes == 42);
        unsigned writes = f.flash.writes;
        assert(boot_diag_publication_progress(&f.owner, 4, 0) == BOOT_DIAG_PUBLICATION_OK);
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_OK);
        assert(f.flash.writes == writes && f.flash.erases == 1);
        for (int after = 0; after < 2; ++after) for (size_t cut = 1; cut <= total; ++cut) {
            init(&f, page, slot); r = request(&f);
            f.flash.cut_call = cut; f.flash.fail_after = after;
            memset(&out, 0xa5, sizeof out); sentinel = out;
            boot_confirm_result_t result = boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out);
            assert(result != BOOT_CONFIRM_OK && !memcmp(&out, &sentinel, sizeof out));
            size_t stopped = f.flash.calls;
            if (result == BOOT_CONFIRM_RECONCILE) {
                assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_RECONCILE);
                assert(f.flash.calls == stopped);
            }
            f.flash.stopped = 0; f.flash.cut_call = SIZE_MAX;
            boot_store_snapshot_t loaded;
            assert(boot_store_load(&f.sources.metadata, &loaded) == BOOT_STORE_OK);
            assert(loaded.record.sequence == 20 || loaded.record.sequence == 21);
            assert(loaded.record.current_slot == (loaded.record.sequence == 20 ? (slot ^ 1u) : slot));
        }
        init(&f, page, slot); r = request(&f); r.trial_id--;
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_REJECTED);
        assert(!f.flash.writes && !f.flash.erases);
        r = request(&f); assert(boot_diag_publication_progress(&f.owner, 4, 3) == BOOT_DIAG_PUBLICATION_OK);
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_REJECTED);
        assert(!f.flash.writes && !f.flash.erases);
    }
}
static void confirmed_zero_no_write(void)
{
    for (unsigned page = 0; page < 2; ++page) for (uint32_t slot = 0; slot < 2; ++slot) {
        fixture_t f; boot_store_snapshot_t loaded, out, sentinel;
        init(&f, page, slot);
        assert(boot_store_load(&f.sources.metadata, &loaded) == BOOT_STORE_OK);
        loaded.record.current_slot = slot;
        loaded.record.previous_slot = slot ^ 1u;
        loaded.record.candidate_slot = BOOT_SLOT_NONE;
        loaded.record.confirmed_mask = BOOT_CONFIRMED_A | BOOT_CONFIRMED_B;
        loaded.record.images[slot].accounting = BOOT_STATE_NONE;
        loaded.record.images[slot].trial_id = 0;
        assert(boot_metadata_encode(&loaded.record, BOOT_COMMIT_VALID,
            f.flash.bytes + page*BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
        uint8_t before[sizeof f.flash.bytes]; memcpy(before, f.flash.bytes, sizeof before);
        assert(boot_diag_publication_progress(&f.owner, 4, 0) == BOOT_DIAG_PUBLICATION_OK);
        boot_confirm_request_t r = request(&f); r.trial_id = 0;
        /* A discarded success reply may be retried without health or a write. */
        for (unsigned retry = 0; retry < 2; ++retry) {
            assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_OK);
            assert(out.record.sequence == loaded.record.sequence);
            assert(out.record.images[slot].trial_id == 0);
            uint8_t encoded[BOOT_METADATA_BYTES];
            assert(boot_metadata_encode(&out.record, BOOT_COMMIT_VALID,
                encoded, sizeof encoded) == BOOT_OK);
            assert(!memcmp(encoded, before + page*BOOT_ERASE_BYTES, sizeof encoded));
        }
        memset(&out, 0xa5, sizeof out); sentinel = out;
        r.trial_id = 1;
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_REJECTED);
        assert(!memcmp(&out, &sentinel, sizeof out));
        r.trial_id = 0; r.payload_hash[0] ^= 1u;
        assert(boot_confirm_apply(&f.service, &f.owner, &f.sources, &r, &out) == BOOT_CONFIRM_REJECTED);
        assert(!memcmp(&out, &sentinel, sizeof out));
        assert(!f.flash.writes && !f.flash.erases);
        assert(!memcmp(before, f.flash.bytes, sizeof before));
    }
}
int main(void)
{
    parser(); parser_decimal_format(); status_render(); durable(); confirmed_zero_no_write();
    puts("boot service checks passed"); return 0;
}
