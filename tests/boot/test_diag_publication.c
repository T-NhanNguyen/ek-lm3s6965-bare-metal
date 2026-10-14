#include "lm3s6965/boot_diag_publication.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t ram[16]; unsigned log[32], used, calls, cut;
    unsigned reads, read_cut;
    bool after, recurse; boot_diag_publication_t *owner;
} model_t;
static bool access_word(void *user, unsigned i, bool write, uint32_t *v)
{
    model_t *m = user;
    assert(i < 16);
    if (!write) {
        ++m->reads;
        if (m->reads == m->read_cut) return false;
        *v = m->ram[i]; return true;
    }
    ++m->calls;
    if (m->calls == m->cut && !m->after) return false;
    m->log[m->used++] = i;
    m->ram[i] = *v;
    if (m->recurse) {
        m->recurse = false;
        assert(boot_diag_publication_boot(m->owner, 0) == BOOT_DIAG_PUBLICATION_BUSY);
    }
    return m->calls != m->cut;
}
static void trace_barrier(void *user)
{
    model_t *m = user; m->log[m->used++] = 16;
}
static const uint32_t initial[16] = {
    0x31444241, 1, 64, 1, 3, 2, 8, 0xdeadbeef,
    0, 1, 0xffffffff, 0, 3, 0, 1, 0
};
static const uint32_t boot[16] = {
    0x31444241, 1, 64, 1, 0, 0, 0, 0x87654321,
    0xffffffff, 0xffffffff, 0, 0, 0, 0, 0xffffffff, 0
};
static const unsigned order[19] = {3,16,0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,3};
static boot_diag_publication_t setup(model_t *m)
{
    boot_diag_publication_t p = {m->ram, access_word, trace_barrier, m,
        {false,false,false,false,false,false}, false, false};
    memset(m, 0, sizeof(*m)); memcpy(m->ram, initial, sizeof(initial)); return p;
}
int main(void)
{
    model_t m; boot_diag_publication_t p = setup(&m);
    boot_diag_context_t unknown = {false,false,false,false,false,false}, c;
    boot_diag_t r;
    unsigned cut, after, garbage, i;
    assert(boot_diag_publication_adopt(&p, &unknown, false) == BOOT_DIAG_PUBLICATION_UNASSOCIATED);
    assert(m.used == 0);
    assert(boot_diag_publication_adopt(&p, &unknown, true) == BOOT_DIAG_PUBLICATION_OK);
    assert(memcmp(m.ram, initial, sizeof(initial)) == 0 && m.used == 0);
    assert(boot_diag_publication_progress(&p, 6, 0) == BOOT_DIAG_PUBLICATION_INVALID);
    assert(boot_diag_publication_progress(&p, 4, 8) == BOOT_DIAG_PUBLICATION_INVALID);
    assert(m.used == 0);
    m.owner = &p; m.recurse = true;
    assert(boot_diag_publication_progress(&p, 4, 7) == BOOT_DIAG_PUBLICATION_OK);
    assert(m.used == 19 && memcmp(m.log, order, sizeof(order)) == 0);
    for (i = 0; i < 16; ++i) assert(m.ram[i] == (i == 4 ? 4u : i == 13 ? 7u : initial[i]));
    assert(boot_diag_publication_snapshot(&p, &r, &c) == BOOT_DIAG_PUBLICATION_OK);
    assert(!c.sequence_known && !c.trial_id_known && !c.failures_known &&
        !c.stage_known && !c.reset_flags_known && !c.health_known);
    assert(r.fault == 2 && r.detail == 8 && r.failed_slot == 1 &&
        r.reset_flags == 0xdeadbeef && r.failures == 3 && r.sequence == UINT32_MAX);
    assert(boot_diag_publication_snapshot(&p, (boot_diag_t *)m.ram, &c) == BOOT_DIAG_PUBLICATION_ARGUMENT);
    assert(boot_diag_publication_adopt(&p, &p.context, true) == BOOT_DIAG_PUBLICATION_ARGUMENT);
    for (garbage = 0; garbage < 2; ++garbage) for (after = 0; after < 2; ++after)
        for (cut = 1; cut <= 18; ++cut) {
            p = setup(&m);
            if (garbage) memset(m.ram, 0xa5, sizeof(m.ram));
            m.cut = cut; m.after = after != 0;
            assert(boot_diag_publication_boot(&p, 0x87654321) ==
                (cut == 18 ? BOOT_DIAG_PUBLICATION_OK : BOOT_DIAG_PUBLICATION_UNCERTAIN));
            if (cut == 18) {
                assert(memcmp(m.ram, boot, sizeof(boot)) == 0);
                assert(memcmp(m.log, order, sizeof(order)) == 0);
            } else {
                assert(!p.associated);
                if ((cut > 1 && cut < 17) || (cut == 1 && after) || (cut == 17 && !after)) {
                    uint8_t wire[64];
                    assert(m.ram[3] == 0);
                    for (i = 0; i < 16; ++i) {
                        wire[i*4] = (uint8_t)m.ram[i]; wire[i*4+1] = (uint8_t)(m.ram[i] >> 8);
                        wire[i*4+2] = (uint8_t)(m.ram[i] >> 16); wire[i*4+3] = (uint8_t)(m.ram[i] >> 24);
                    }
                    assert(boot_diag_decode(wire, sizeof(wire), BOOT_DIAG_PUBLISHED, &r) != BOOT_DIAG_OK);
                }
                if (cut == 17 && after) assert(m.ram[3] == 1); /* effect uncertain */
                memset(&r, 0xa5, sizeof(r));
                assert(boot_diag_publication_snapshot(&p, &r, &c) == BOOT_DIAG_PUBLICATION_UNASSOCIATED);
                assert(r.stage == 0xa5a5a5a5);
            }
        }
    p = setup(&m); m.ram[5] = 6;
    assert(boot_diag_publication_adopt(&p, &unknown, true) == BOOT_DIAG_PUBLICATION_INVALID);
    assert(m.used == 0);
    for (cut = 1; cut <= 16; ++cut) {
        p = setup(&m); m.read_cut = cut;
        assert(boot_diag_publication_adopt(&p, &unknown, true) == BOOT_DIAG_PUBLICATION_READ);
        assert(m.used == 0 && !p.associated);
        p = setup(&m);
        assert(boot_diag_publication_adopt(&p, &unknown, true) == BOOT_DIAG_PUBLICATION_OK);
        m.reads = 0; m.read_cut = cut;
        assert(boot_diag_publication_progress(&p, 4, 7) == BOOT_DIAG_PUBLICATION_READ);
        assert(m.used == 0 && memcmp(m.ram, initial, sizeof(initial)) == 0);
        m.reads = 0;
        memset(&r, 0xa5, sizeof(r));
        assert(boot_diag_publication_snapshot(&p, &r, &c) == BOOT_DIAG_PUBLICATION_READ);
        assert(r.stage == 0xa5a5a5a5);
    }
    for (i = 4; i < 16; ++i) {
        if (i == 7 || i == 10 || i == 11) continue; /* full uint32 domains */
        p = setup(&m); m.ram[i] = 0xfffffffe;
        assert(boot_diag_publication_adopt(&p, &unknown, true) == BOOT_DIAG_PUBLICATION_INVALID);
        assert(m.used == 0);
    }
    p = setup(&m); p.words = (volatile uint32_t *)((uint8_t *)m.ram + 1);
    assert(boot_diag_publication_boot(&p, 0) == BOOT_DIAG_PUBLICATION_ARGUMENT);
    assert(m.used == 0);
    puts("diagnostic publication tests passed"); return 0;
}
