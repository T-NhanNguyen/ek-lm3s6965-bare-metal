/* Standalone native report regression. Reuse the signed transport fixture and
 * device model, not another verifier or invented authenticated identity. */
#include "../ethernet/ftp/ftp_boot_fixture.c"

static const ftp_boot_report_descriptor_t reports[] = {
    { "slot-a-breathing", "Breathing LED with FTP service." },
    { "slot-b-blink", "Double-blink LED with FTP service." }
};
static bool retrieve(ftp_boot_t *b, const char *path, ftp_storage_view_t *view)
{
    return b->operation.ops->begin(b, path, strlen(path), false, view);
}
static void finished(ftp_boot_t *b) { b->operation.ops->end(b); }
static void only_reads(unsigned from)
{
    /* Controller idle polling reads registers before flash reads. */
    for (unsigned i = from; i < m.calls; ++i) assert(m.events[i].kind != 1);
    assert(m.commands == 0);
    assert(m.receiver.phase == BOOT_RECEIVE_NEW);
    assert(!binding.reset_requested && !binding.accepted);
}
static void reports_fresh(void)
{
    for (uint32_t slot = 0; slot < 2; ++slot) {
        ftp_boot_t *b = fixture_init(slot, false, &reports[slot]);
        for (unsigned round = 0; round < 2; ++round) {
            /* Source authority changes between requests, never during capture. */
            m.initial.sequence = 20u + round;
            encode_initial();
            boot_snapshot_t snapshot;
            assert(boot_snapshot_capture(&publication, &b->config.sources, &snapshot) == BOOT_SNAPSHOT_OK);
            char expected[FTP_BOOT_REPORT_CAPACITY], legacy[BOOT_STATUS_CAPACITY];
            size_t legacy_length;
            assert(boot_status_render(&snapshot, legacy, sizeof legacy, &legacy_length));
            int n = snprintf(expected, sizeof expected,
                "format=1\napplication=%s\nmessage=%s booted successfully. %s\n\n",
                reports[slot].id, reports[slot].id, reports[slot].description);
            assert(n > 0);
            memcpy(expected + n, legacy, legacy_length);
            unsigned captures = executing_captures, from = m.calls;
            uint32_t words[16]; memcpy(words, diagnostic_words, sizeof words);
            ftp_storage_view_t view = {0};
            assert(retrieve(b, "/boot-report", &view));
            assert(executing_captures == captures + 1);
            assert(view.length == (size_t)n + legacy_length);
            assert(!memcmp(view.data, expected, view.length));
            finished(b);
            assert(retrieve(b, "boot-status", &view));
            assert(view.length == legacy_length && !memcmp(view.data, legacy, legacy_length));
            finished(b);
            assert(!memcmp(words, diagnostic_words, sizeof words));
            only_reads(from);
        }
    }
}
static void reports_trial(void)
{
    for (uint32_t slot = BOOT_SLOT_A; slot <= BOOT_SLOT_B; ++slot) {
        ftp_boot_t *b = fixture_init(slot, false, &reports[slot]);
        uint32_t accepted = slot ^ 1u;
        /* Install and authenticate the retained accepted image independently;
         * copying the running trial's identity would invent signed history. */
        payload(m.bytes + base(accepted), accepted, 0);
        memcpy(m.bytes + manifest_address(accepted), fixture_manifest[accepted * 7u],
            BOOT_MANIFEST_BYTES);
        boot_verified_image_t image;
        assert(boot_verify_stored(m.bytes + manifest_address(accepted), BOOT_MANIFEST_BYTES,
            accepted, image_read, &m, NULL, NULL, &image) == BOOT_VERIFY_OK);
        m.initial.images[accepted] = (boot_image_entry_t){
            .payload_length = image.manifest.payload_length,
            .release = image.manifest.release, .trial_id = 9
        };
        memcpy(m.initial.images[accepted].payload_hash, image.manifest.payload_hash, 32);
        m.initial.current_slot = accepted;
        m.initial.candidate_slot = slot;
        m.initial.confirmed_mask = 1u << accepted;
        m.initial.images[slot].accounting = BOOT_STATE_TRIAL;
        m.initial.images[slot].trial_id = m.initial.sequence;
        encode_initial();

        /* The watchdog remains application-owned with genuine partial evidence;
         * report retrieval must neither feed it nor change its ownership/state. */
        watchdog_io_t io = {0}; boot_watchdog_t watchdog;
        boot_watchdog_io_t callbacks = {&io, watchdog_read, watchdog_write};
        assert(boot_watchdog_init(&watchdog, &callbacks) == BOOT_WATCHDOG_OK);
        assert(boot_watchdog_configure(&watchdog, 8, 40) == BOOT_WATCHDOG_OK);
        assert(boot_watchdog_boot_report(&watchdog, 1, 7, BOOT_VERIFY_MANIFEST_BEGIN,
            0, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
        assert(boot_watchdog_boot_report(&watchdog, 1, 7, BOOT_VERIFY_MANIFEST_AUTHENTICATED,
            0, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
        assert(boot_watchdog_transfer(&watchdog, 1) == BOOT_WATCHDOG_OK);
        assert(boot_watchdog_application_report(&watchdog, 2, BOOT_WATCHDOG_ETHERNET, 1)
            == BOOT_WATCHDOG_OK);
        boot_watchdog_t watchdog_before;
        watchdog_io_t io_before;
        memcpy(&watchdog_before, &watchdog, sizeof watchdog);
        memcpy(&io_before, &io, sizeof io);
        uint8_t flash_before[sizeof m.bytes];
        memcpy(flash_before, m.bytes, sizeof flash_before);
        uint32_t words[16]; memcpy(words, diagnostic_words, sizeof words);
        unsigned from = m.calls;

        boot_snapshot_t snapshot;
        assert(boot_snapshot_capture(&publication, &b->config.sources, &snapshot) == BOOT_SNAPSHOT_OK);
        assert(snapshot.running.manifest.slot == slot);
        assert(snapshot.metadata.record.current_slot == accepted);
        assert(snapshot.metadata.record.candidate_slot == slot);
        assert(snapshot.metadata.record.images[slot].trial_id == snapshot.metadata.record.sequence);
        assert(snapshot.diagnostic_context.health_known && snapshot.diagnostic.health == 7);
        assert(snapshot.diagnostic.fault == BOOT_DIAG_FAULT_NONE);
        char legacy[BOOT_STATUS_CAPACITY], expected[FTP_BOOT_REPORT_CAPACITY];
        size_t length;
        assert(boot_status_render(&snapshot, legacy, sizeof legacy, &length));
        char hash[65], identity[256];
        for (unsigned i = 0; i < 32; ++i)
            assert(snprintf(hash + i * 2u, 3, "%02x",
                (unsigned)executing.manifest.payload_hash[i]) == 2);
        int identity_length = snprintf(identity, sizeof identity,
            "running_slot=%c\nhash=%s\nrelease=%lu\nconfirmed_slot=%c\ncandidate_slot=%c\n"
            "permitted_slot=none\ntrial_id=20\ntrial_state=trial\n",
            slot == BOOT_SLOT_A ? 'A' : 'B', hash, (unsigned long)executing.manifest.release,
            accepted == BOOT_SLOT_A ? 'A' : 'B', slot == BOOT_SLOT_A ? 'A' : 'B');
        assert(identity_length > 0 && (size_t)identity_length < sizeof identity);
        assert(length < sizeof legacy); legacy[length] = 0;
        assert(strstr(legacy, identity));
        int n = snprintf(expected, sizeof expected,
            "format=1\napplication=%s\nmessage=%s booted successfully. %s\n\n",
            reports[slot].id, reports[slot].id, reports[slot].description);
        assert(n > 0 && (size_t)n + length <= sizeof expected);
        memcpy(expected + n, legacy, length);
        unsigned captures = executing_captures;
        ftp_storage_view_t view = {0};
        assert(retrieve(b, "boot-report", &view));
        assert(executing_captures == captures + 1);
        assert(view.length == (size_t)n + length && !memcmp(view.data, expected, view.length));
        finished(b);
        assert(retrieve(b, "boot-status", &view));
        assert(view.length == length && !memcmp(view.data, legacy, length));
        finished(b);
        assert(!memcmp(flash_before, m.bytes, sizeof flash_before));
        assert(!memcmp(words, diagnostic_words, sizeof words));
        assert(!memcmp(&watchdog_before, &watchdog, sizeof watchdog));
        assert(!memcmp(&io_before, &io, sizeof io));
        only_reads(from);
    }
}
static void refused(void)
{
    ftp_storage_view_t view = {0};
    ftp_boot_t *b = ftp_boot_fixture_init(false);
    unsigned from = m.calls;
    assert(!retrieve(b, "boot-report", &view));
    assert(m.calls == from && executing_captures == 0);
    assert(retrieve(b, "boot-status", &view)); finished(b);
    b = fixture_init(BOOT_SLOT_A, false, &reports[0]);
    for (uint32_t health = 0; health < BOOT_HEALTH_REQUIRED; ++health) {
        assert(boot_diag_publication_progress(&publication, 4, health) == BOOT_DIAG_PUBLICATION_OK);
        from = m.calls;
        assert(!retrieve(b, "boot-report", &view));
        assert(retrieve(b, "boot-status", &view)); finished(b);
        only_reads(from);
    }
    assert(boot_diag_publication_progress(&publication, 4, BOOT_HEALTH_REQUIRED) == BOOT_DIAG_PUBLICATION_OK);
    publication.context.health_known = false;
    assert(!retrieve(b, "boot-report", &view));
    b = fixture_init(BOOT_SLOT_A, false, &reports[1]);
    assert(!retrieve(b, "boot-report", &view)); /* descriptor cannot rename A */
    b = fixture_init(BOOT_SLOT_A, false, &reports[0]);
    boot_diag_t diag;
    boot_diag_context_t context;
    assert(boot_diag_publication_snapshot(&publication, &diag, &context) == BOOT_DIAG_PUBLICATION_OK);
    diag.fault = BOOT_DIAG_FAULT_STARTUP;
    diag.detail = BOOT_DIAG_DETAIL_FTP;
    uint8_t wire[BOOT_DIAG_BYTES];
    assert(boot_diag_encode(&diag, BOOT_DIAG_PUBLISHED, wire, sizeof wire) == BOOT_DIAG_OK);
    memcpy(diagnostic_words, wire, sizeof wire);
    assert(!retrieve(b, "boot-report", &view));
    b = fixture_init(BOOT_SLOT_A, false, &reports[0]);
    ftp_boot_fixture_fail();
    assert(!retrieve(b, "boot-report", &view)); /* failed fresh metadata read */
    b = fixture_init(BOOT_SLOT_A, false, &reports[0]);
    executing.manifest.release++; /* stale/mismatched authenticated source */
    assert(!retrieve(b, "boot-report", &view));
}
static void malformed(void)
{
    ftp_boot_t *b = fixture_init(BOOT_SLOT_A, false, NULL);
    ftp_boot_config_t c = b->config;
    char over[FTP_BOOT_REPORT_DESCRIPTION_MAX + 2u];
    memset(over, 'x', sizeof over); over[sizeof over - 1u] = 0;
    const ftp_boot_report_descriptor_t bad[] = {
        { NULL, "text" }, { "", "text" }, { "unknown", "text" },
        { "slot-a-breathing", NULL }, { "slot-a-breathing", "" },
        { "slot-a-breathing", "line\ninjection" },
        { "slot-a-breathing", "carriage\rreturn" },
        { "slot-a-breathing", "\177" }, { "slot-a-breathing", "\200" },
        { "slot-a-breathing", over }
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        ftp_boot_t fresh = {0}; c.report = &bad[i];
        assert(!ftp_boot_init(&fresh, &c));
        assert(!fresh.operation.ops);
    }
    /* Exact maximum fits with full status; no truncation at the boundary. */
    over[FTP_BOOT_REPORT_DESCRIPTION_MAX] = 0;
    ftp_boot_report_descriptor_t max = { "slot-a-breathing", over };
    ftp_boot_t fresh = {0}; c.report = &max;
    assert(ftp_boot_init(&fresh, &c));
    ftp_storage_view_t view;
    assert(retrieve(&fresh, "boot-report", &view)); finished(&fresh);
}
int main(void)
{
    reports_fresh(); reports_trial(); refused(); malformed();
    puts("ftp boot-report: ok");
    return 0;
}
