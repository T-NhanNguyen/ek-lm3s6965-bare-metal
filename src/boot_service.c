#include "lm3s6965/boot_service.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lm3s6965/boot_diag.h"
#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_snapshot.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

static bool decimal(const char *s, uint32_t *out)
{
    uint32_t n = 0;
    if (!*s) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        uint32_t d = (uint32_t)(*s - '0');
        if (n > (UINT32_MAX - d) / 10u) return false;
        n = n * 10u + d;
    }
    *out = n;
    return true;
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
bool boot_confirm_feed(boot_confirm_parser_t *p, const uint8_t *bytes, size_t n)
{
    if (!p || p->failed || p->finished) return false;
    if ((!bytes && n) || p->used > BOOT_CONFIRM_CAPACITY ||
        n > BOOT_CONFIRM_CAPACITY - p->used) { p->failed = true; return false; }
    for (size_t i = 0; i < n; ++i) {
        if (bytes[i] != '\n' && (bytes[i] < 32 || bytes[i] > 126)) {
            p->failed = true; return false;
        }
    }
    if (n) memcpy(p->bytes + p->used, bytes, n);
    p->used += n;
    return true;
}
bool boot_confirm_finish(boot_confirm_parser_t *p, boot_confirm_request_t *out)
{
    boot_confirm_request_t r = {0};
    char text[BOOT_CONFIRM_CAPACITY + 1u];
    unsigned seen = 0;
    if (!p || !out || p->failed || p->finished) return false;
    p->finished = true;
    if (!p->used || p->used > BOOT_CONFIRM_CAPACITY || p->bytes[p->used-1] != '\n') return false;
    memcpy(text, p->bytes, p->used); text[p->used] = 0;
    char *line = text;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) return false;
        *end = 0;
        char *value = strchr(line, '=');
        if (!value) return false;
        *value++ = 0;
        unsigned bit;
        if (!strcmp(line, "format")) {
            uint32_t format;
            bit = 1;
            if (!decimal(value, &format) || format != 1u) return false;
        }
        else if (!strcmp(line, "slot")) {
            bit = 2;
            if (!strcmp(value, "A")) r.slot = BOOT_SLOT_A;
            else if (!strcmp(value, "B")) r.slot = BOOT_SLOT_B;
            else return false;
        } else if (!strcmp(line, "hash")) {
            bit = 4;
            if (strlen(value) != 64) return false;
            for (size_t i = 0; i < 32; ++i) {
                int a = nibble(value[2*i]), b = nibble(value[2*i+1]);
                if (a < 0 || b < 0) return false;
                r.payload_hash[i] = (uint8_t)(a*16+b);
            }
        } else if (!strcmp(line, "trial_id")) {
            bit = 8; if (!decimal(value, &r.trial_id)) return false;
        } else return false;
        if (seen & bit) return false;
        seen |= bit; line = end + 1;
    }
    if (seen != 15) return false;
    *out = r; return true;
}
static const char *slot(uint32_t s)
{ return s == BOOT_SLOT_A ? "A" : s == BOOT_SLOT_B ? "B" : "none"; }
static void number(char *s, uint32_t n)
{ (void)snprintf(s, 11, "%" PRIu32, n); }
static void reasons(const boot_image_entry_t *e, char *out)
{
    static const char *const names[] = {"none", "watchdog", "external", "brownout", "network_deadline", "combined"};
    unsigned count = (e->accounting >> BOOT_ACCOUNT_COUNT_SHIFT) & 3u;
    out[0] = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned r = (e->accounting >> (5u + i*3u)) & 7u;
        if (i) strcat(out, ",");
        strcat(out, names[r]);
    }
    if (!count) strcpy(out, "none");
}
bool boot_status_render(const boot_snapshot_t *s, char *out, size_t capacity, size_t *size)
{
    static const char *const states[] = {"none", "ready", "trial", "rejected"};
    char body[BOOT_STATUS_CAPACITY], hash[65], seq[11], release[11], trial[11];
    char failures[2][11], rollback[2][11], why[2][51], health[9], reset[9], stage[11];
    if (!s || !out || !size || boot_metadata_validate(&s->metadata.record) != BOOT_OK ||
        boot_diag_validate(&s->diagnostic) != BOOT_DIAG_OK ||
        boot_manifest_validate(&s->running.manifest) != BOOT_OK ||
        boot_metadata_image_matches(&s->metadata.record, &s->running.manifest) != BOOT_OK) return false;
    const boot_metadata_t *m = &s->metadata.record;
    const boot_manifest_t *running = &s->running.manifest;
    uint32_t source = m->candidate_slot != BOOT_SLOT_NONE ? m->candidate_slot : running->slot;
    uint32_t permitted = BOOT_SLOT_NONE;
    if (m->candidate_slot == BOOT_SLOT_NONE && m->current_slot == running->slot &&
        (m->confirmed_mask & (1u << running->slot))) permitted = running->slot ^ 1u;
    for (size_t i = 0; i < 32; ++i) (void)snprintf(hash+2*i, 3, "%02x", (unsigned)running->payload_hash[i]);
    number(seq, m->sequence); number(release, running->release); number(trial, m->images[source].trial_id);
    for (unsigned i = 0; i < 2; ++i) {
        const boot_image_entry_t *e = &m->images[i];
        if (!e->payload_length) { strcpy(failures[i], "none"); strcpy(rollback[i], "none"); strcpy(why[i], "none"); }
        else {
            number(failures[i], (e->accounting >> 2) & 3u);
            number(rollback[i], (e->accounting & BOOT_ACCOUNT_ROLLBACK) ? 1u : 0u);
            reasons(e, why[i]);
        }
    }
    strcpy(health, "none"); strcpy(reset, "none"); strcpy(stage, "none");
    if (s->diagnostic_context.health_known) (void)snprintf(health, sizeof health, "%08" PRIx32, s->diagnostic.health);
    if (s->diagnostic_context.reset_flags_known) (void)snprintf(reset, sizeof reset, "%08" PRIx32, s->diagnostic.reset_flags);
    if (s->diagnostic_context.stage_known) number(stage, s->diagnostic.stage);
    int n = snprintf(body, sizeof body,
        "format=1\nsequence=%s\nrunning_slot=%s\nhash=%s\nrelease=%s\nconfirmed_slot=%s\ncandidate_slot=%s\npermitted_slot=%s\ntrial_id=%s\ntrial_state=%s\n"
        "slot_a_failures=%s\nslot_a_reasons=%s\nslot_a_rollback=%s\nslot_b_failures=%s\nslot_b_reasons=%s\nslot_b_rollback=%s\nhealth=%s\nstage=%s\nfault=%" PRIu32 "\ndetail=%" PRIu32 "\nreset_flags=%s\n",
        seq, slot(running->slot), hash, release, slot(m->current_slot), slot(m->candidate_slot), slot(permitted), trial,
        states[m->images[source].accounting & 3u], failures[0], why[0], rollback[0], failures[1], why[1], rollback[1],
        health, stage, s->diagnostic.fault, s->diagnostic.detail, reset);
    if (n < 0 || (size_t)n >= sizeof body || (size_t)n > capacity) return false;
    memcpy(out, body, (size_t)n); *size = (size_t)n; return true;
}
boot_confirm_result_t boot_confirm_apply(boot_confirm_service_t *service,
    boot_diag_publication_t *owner, const boot_snapshot_sources_t *sources,
    const boot_confirm_request_t *request, boot_store_snapshot_t *out)
{
    boot_snapshot_t snapshot;
    boot_metadata_t desired;
    boot_store_snapshot_t committed;
    boot_confirm_result_t result = BOOT_CONFIRM_SNAPSHOT;
    if (!service || !owner || !sources || !request || !out ||
        !sources->metadata.erase_page || !sources->metadata.program_word) return BOOT_CONFIRM_ARGUMENT;
    if (service->busy) return BOOT_CONFIRM_BUSY;
    if (service->reconcile) return BOOT_CONFIRM_RECONCILE;
    service->busy = true;
    boot_confirm_request_t frozen = *request;
    if (boot_snapshot_capture(owner, sources, &snapshot) != BOOT_SNAPSHOT_OK) goto done;
    uint32_t health = snapshot.diagnostic_context.health_known ? snapshot.diagnostic.health : 0;
    boot_lifecycle_result_t proposal = boot_lifecycle_confirm(&snapshot.metadata.record, &frozen,
        &snapshot.running, health, &desired);
    if (proposal == BOOT_LIFECYCLE_NO_CHANGE) { committed = snapshot.metadata; result = BOOT_CONFIRM_OK; }
    else if (proposal != BOOT_LIFECYCLE_CHANGE) result = BOOT_CONFIRM_REJECTED;
    else {
        boot_store_result_t store = boot_store_replace(&sources->metadata, &snapshot.metadata, &desired, &committed);
        if (store == BOOT_STORE_UNCERTAIN) { service->reconcile = true; result = BOOT_CONFIRM_RECONCILE; }
        else result = store == BOOT_STORE_OK ? BOOT_CONFIRM_OK : BOOT_CONFIRM_STORE;
    }
    if (result == BOOT_CONFIRM_OK) *out = committed;
done:
    service->busy = false;
    return result;
}
