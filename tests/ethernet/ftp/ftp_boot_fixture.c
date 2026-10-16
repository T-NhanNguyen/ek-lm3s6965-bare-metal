/* Reuse the independently signed receive fixtures and register device model.
 * The renamed legacy entry point is compiled, NEVER called here. This avoids
 * a second, weaker fake verifier/store/controller in the transport proof. */
#include "ftp_boot_fixture.h"
int ftp_boot_receive_regressions_not_run(void);
#define main ftp_boot_receive_regressions_not_run
#include "../../../tests/boot/test_receive.c"
#undef main
static ftp_boot_t binding;
static boot_verified_image_t executing;
static uint32_t diagnostic_words[16];
static boot_diag_publication_t publication;
static int metadata_read(void *ctx, uint32_t address, uint8_t *p, size_t n)
{ (void)ctx; return boot_flash_read(&m.flash, address, p, n) != BOOT_FLASH_OK; }
static int metadata_erase(void *ctx, uint32_t address)
{ (void)ctx; return boot_flash_erase(&m.flash, address) != BOOT_FLASH_OK; }
static int metadata_program(void *ctx, uint32_t address, uint32_t word)
{ (void)ctx; return boot_flash_program(&m.flash, address, word) != BOOT_FLASH_OK; }
static int executing_source(void *ctx, boot_verified_image_t *out)
{ (void)ctx; *out = executing; return 0; }
ftp_boot_t *ftp_boot_fixture_init(bool trial)
{
    setup(BOOT_SLOT_B, 0, 1);
    /* Fresh independent device case; not a production reinitialization. */
    memset(&binding, 0, sizeof binding);
    memset(&publication, 0, sizeof publication);
    memset(diagnostic_words, 0, sizeof diagnostic_words);
    publication.words = diagnostic_words;
    assert(boot_diag_publication_boot(&publication, 0) == BOOT_DIAG_PUBLICATION_OK);
    assert(boot_diag_publication_progress(&publication, 4, 7) == BOOT_DIAG_PUBLICATION_OK);
    assert(running(&m, &executing) == 0);
    if (trial) {
        /* Executing image is real authenticated A, now an unconfirmed trial. */
        m.initial.current_slot = BOOT_SLOT_NONE;
        m.initial.candidate_slot = BOOT_SLOT_A;
        m.initial.confirmed_mask = 0;
        m.initial.images[0].accounting = BOOT_STATE_TRIAL;
        m.initial.images[0].trial_id = 19;
        encode_initial();
    }
    ftp_boot_config_t c = { .receiver = &m.receiver, .receive = config(),
        .publication = &publication,
        .sources = { .metadata = { &m, metadata_read, metadata_erase, metadata_program },
            .running_context = &m, .running = executing_source } };
    assert(ftp_boot_init(&binding, &c));
    m.calls = 0;
    return &binding;
}
size_t ftp_boot_fixture_package(uint8_t *out)
{
    memcpy(out, fixture_manifest[7], BOOT_MANIFEST_BYTES);
    payload(out + BOOT_MANIFEST_BYTES, BOOT_SLOT_B, 0);
    return BOOT_MANIFEST_BYTES + BOOT_PAYLOAD_MIN;
}
size_t ftp_boot_fixture_confirm(char *out)
{
    static const char hex[] = "0123456789abcdef";
    char hash[65];
    for (unsigned i = 0; i < 32; ++i) {
        hash[i*2] = hex[executing.manifest.payload_hash[i] >> 4];
        hash[i*2+1] = hex[executing.manifest.payload_hash[i] & 15];
    }
    hash[64] = 0;
    int n = snprintf(out, BOOT_CONFIRM_CAPACITY, "format=1\nslot=A\nhash=%s\ntrial_id=19\n", hash);
    assert(n > 0 && n < (int)BOOT_CONFIRM_CAPACITY);
    return (size_t)n;
}
void ftp_boot_fixture_fail(void) { m.fail = m.calls + 1; }
boot_store_snapshot_t ftp_boot_fixture_metadata(void)
{
    boot_store_snapshot_t out;
    boot_store_adapter_t a = { &m, metadata_read, metadata_erase, metadata_program };
    assert(boot_store_load(&a, &out) == BOOT_STORE_OK);
    return out;
}
unsigned ftp_boot_fixture_commands(void) { return m.commands; }
