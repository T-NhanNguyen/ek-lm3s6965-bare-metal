#include "lm3s6965/boot_snapshot.h"
#include <string.h>
static boot_diag_publication_t owner;
static volatile unsigned isr_result;
static uint8_t metadata_pages[2][BOOT_METADATA_BYTES];
static unsigned capture_reads, capture_runs;
static bool capture_guard_ok;
static boot_verified_image_t executing;
static boot_snapshot_sources_t sources;
static void capture_guard(void)
{
    uint32_t v;
    __asm__ volatile("mrs %0, primask" : "=r"(v));
    capture_guard_ok &= v == 1 && owner.busy;
    capture_guard_ok &= boot_diag_publication_progress(&owner, 0, 0) == BOOT_DIAG_PUBLICATION_BUSY;
}
static int metadata_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    (void)context; capture_guard(); ++capture_reads;
    if (size != BOOT_METADATA_BYTES ||
        (address != BOOT_METADATA_PAGE0 && address != BOOT_METADATA_PAGE1)) return -1;
    memcpy(bytes, metadata_pages[address == BOOT_METADATA_PAGE0 ? 0 : 1], size);
    return 0;
}
static int running_read(void *context, boot_verified_image_t *out)
{
    (void)context; capture_guard(); ++capture_runs; *out = executing; return 0;
}
void svc_handler(void);
void fixture_main(void);
static uint32_t primask(void)
{
    uint32_t v; __asm__ volatile("mrs %0, primask" : "=r"(v)); return v;
}
static void mask(uint32_t v) { __asm__ volatile("msr primask, %0" :: "r"(v) : "memory"); }
static void finish(bool ok)
{
    const char *s = ok ? "PUBLICATION PASS\n" : "PUBLICATION FAIL\n";
    register uint32_t r0 __asm__("r0") = 4;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    uint32_t args[2] = {0x20026, ok ? 0u : 1u};
    r0 = 0x20;
    register const uint32_t *exit_arg __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(exit_arg) : "memory");
    for (;;) {}
}
void svc_handler(void)
{
    boot_diag_context_t c = {false,false,false,false,false,false}, out;
    boot_diag_t r;
    bool rejected = boot_diag_publication_boot(&owner, 0) == BOOT_DIAG_PUBLICATION_ISR;
    rejected &= boot_diag_publication_progress(&owner, 0, 0) == BOOT_DIAG_PUBLICATION_ISR;
    rejected &= boot_diag_publication_adopt(&owner, &c, true) == BOOT_DIAG_PUBLICATION_ISR;
    rejected &= boot_diag_publication_snapshot(&owner, &r, &out) == BOOT_DIAG_PUBLICATION_ISR;
    boot_snapshot_t snapshot;
    rejected &= boot_snapshot_capture(&owner, &sources, &snapshot) == BOOT_SNAPSHOT_PUBLICATION;
    isr_result = rejected ? BOOT_DIAG_PUBLICATION_ISR : BOOT_DIAG_PUBLICATION_OK;
}
void fixture_main(void)
{
    volatile uint32_t *ram = (volatile uint32_t *)0x2000dfc0;
    boot_diag_context_t c = {false,false,false,false,false,false}, out;
    boot_diag_t r;
    bool ok = true;
    uint32_t before[16];
    unsigned i, state;
    owner.words = ram;
    boot_metadata_t metadata = {0};
    boot_snapshot_t snapshot;
    metadata.sequence = 19; metadata.current_slot = BOOT_SLOT_A;
    metadata.candidate_slot = metadata.previous_slot = metadata.intent_slot = BOOT_SLOT_NONE;
    metadata.confirmed_mask = BOOT_CONFIRMED_A;
    metadata.images[0].payload_length = BOOT_PAYLOAD_MIN;
    memset(metadata_pages, 0xff, sizeof metadata_pages);
    ok &= boot_metadata_encode(&metadata, BOOT_COMMIT_VALID, metadata_pages[0],
        BOOT_METADATA_BYTES) == BOOT_OK;
    executing.manifest.slot = BOOT_SLOT_A;
    executing.manifest.execution_base = BOOT_SLOT_A_BASE;
    executing.manifest.payload_length = BOOT_PAYLOAD_MIN;
    executing.initial_msp = BOOT_INITIAL_MSP;
    executing.reset_vector = BOOT_SLOT_A_BASE | 1u;
    sources.metadata.read = metadata_read; sources.running = running_read;
    capture_guard_ok = true;
    for (i = 0; i < 16; ++i) ram[i] = 0xa5a5a5a5;
    for (state = 0; state < 2; ++state) {
        mask(state);
        ok &= boot_diag_publication_boot(&owner, 0xdeadbeef) == BOOT_DIAG_PUBLICATION_OK;
        ok &= primask() == state;
        ok &= ram[0] == 0x31444241 && ram[1] == 1 && ram[2] == 64 && ram[3] == 1 &&
            ram[4] == 0 && ram[5] == 0 && ram[6] == 0 && ram[7] == 0xdeadbeef &&
            ram[8] == UINT32_MAX && ram[9] == UINT32_MAX && ram[10] == 0 && ram[11] == 0 &&
            ram[12] == 0 && ram[13] == 0 && ram[14] == UINT32_MAX && ram[15] == 0;
        ok &= boot_diag_publication_progress(&owner, 6, 0) == BOOT_DIAG_PUBLICATION_INVALID;
        ok &= primask() == state && ram[4] == 0;
        ram[5] = 6;
        ok &= boot_diag_publication_adopt(&owner, &c, true) == BOOT_DIAG_PUBLICATION_INVALID;
        ok &= primask() == state;
        ram[5] = 0;
        /* Literal latched fixture, test-only setup before adoption. */
        ram[5] = 2; ram[6] = 8; ram[12] = 3; ram[14] = 1; ram[10] = UINT32_MAX;
        owner.associated = false;
        ok &= boot_diag_publication_adopt(&owner, &c, true) == BOOT_DIAG_PUBLICATION_OK;
        ok &= ram[5] == 2 && ram[6] == 8 && ram[12] == 3 && ram[14] == 1;
        ok &= boot_diag_publication_progress(&owner, 4, 7) == BOOT_DIAG_PUBLICATION_OK;
        ok &= primask() == state;
        ok &= boot_diag_publication_snapshot(&owner, &r, &out) == BOOT_DIAG_PUBLICATION_OK;
        ok &= r.stage == 4 && r.health == 7 && r.fault == 2 && r.detail == 8 &&
            r.failures == 3 && r.failed_slot == 1 && r.reset_flags == 0xdeadbeef &&
            r.sequence == UINT32_MAX && !out.sequence_known && !out.stage_known;
        ok &= boot_snapshot_capture(&owner, &sources, &snapshot) == BOOT_SNAPSHOT_OK;
        ok &= primask() == state && !owner.busy && capture_guard_ok;
        ok &= snapshot.metadata.record.sequence == 19 &&
            snapshot.running.manifest.slot == BOOT_SLOT_A &&
            snapshot.diagnostic.sequence == UINT32_MAX && snapshot.diagnostic.failed_slot == 1;
        boot_snapshot_t frozen = snapshot;
        executing.manifest.release = 1;
        ok &= boot_snapshot_capture(&owner, &sources, &snapshot) == BOOT_SNAPSHOT_IDENTITY;
        ok &= primask() == state && !owner.busy &&
            memcmp(&snapshot, &frozen, sizeof snapshot) == 0;
        executing.manifest.release = 0;
        metadata_pages[0][0] ^= 1;
        ok &= boot_snapshot_capture(&owner, &sources, &snapshot) == BOOT_SNAPSHOT_METADATA;
        ok &= primask() == state && !owner.busy &&
            memcmp(&snapshot, &frozen, sizeof snapshot) == 0;
        metadata_pages[0][0] ^= 1;
    }
    ok &= capture_reads == 12 && capture_runs == 4;
    for (i = 0; i < 16; ++i) before[i] = ram[i];
    mask(0);
    __asm__ volatile("svc 0" ::: "memory");
    for (i = 0; i < 16; ++i) ok &= ram[i] == before[i];
    ok &= capture_reads == 12 && capture_runs == 4;
    ok &= isr_result == BOOT_DIAG_PUBLICATION_ISR && primask() == 0 &&
        ram[3] == 1 && ram[4] == 4 && ram[5] == 2 && ram[7] == 0xdeadbeef && ram[12] == 3;
    finish(ok);
}
