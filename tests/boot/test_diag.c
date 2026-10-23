#include "lm3s6965/boot_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
/* Independent literal byte oracle: all sixteen words at approved offsets. */
static const uint8_t golden[64] = {
    0x41,0x42,0x44,0x31, 1,0,0,0, 64,0,0,0, 1,0,0,0,
    5,0,0,0, 5,0,0,0, 7,0,0,0, 0x78,0x56,0x34,0x12,
    1,0,0,0, 0,0,0,0, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
    3,0,0,0, 7,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0
};
static const boot_diag_t fixture = {5,5,7,UINT32_C(0x12345678),1,0,
    UINT32_MAX,UINT32_MAX,3,7,UINT32_MAX};
static void raw_word(uint8_t *wire, size_t offset, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4u; ++i) {
        wire[offset + i] = (uint8_t)(value % 256u);
        value /= 256u;
    }
}
static void equal(const boot_diag_t *a, const boot_diag_t *b)
{
    CHECK(a->stage == b->stage && a->fault == b->fault && a->detail == b->detail);
    CHECK(a->reset_flags == b->reset_flags && a->running_slot == b->running_slot);
    CHECK(a->selected_slot == b->selected_slot && a->sequence == b->sequence);
    CHECK(a->trial_id == b->trial_id && a->failures == b->failures);
    CHECK(a->health == b->health && a->failed_slot == b->failed_slot);
}
static void malformed(size_t offset, uint32_t value, boot_diag_result_t error)
{
    uint8_t wire[64];
    boot_diag_t out = fixture, before = out;
    memcpy(wire, golden, sizeof wire);
    raw_word(wire, offset, value);
    CHECK(boot_diag_decode(wire, sizeof wire, BOOT_DIAG_PUBLISHED, &out) == error);
    equal(&out, &before);
}
static void golden_and_bounds(void)
{
    uint8_t wire[66], before[66];
    boot_diag_t out;
    size_t n;
    memset(wire, 0xa5, sizeof wire);
    CHECK(BOOT_DIAGNOSTIC_BASE == UINT32_C(0x2000dfc0));
    CHECK(BOOT_DIAG_END == UINT32_C(0x2000e000));
    CHECK(BOOT_DIAG_END - BOOT_DIAGNOSTIC_BASE == 64u);
    CHECK(boot_diag_encode(&fixture, BOOT_DIAG_PUBLISHED, wire + 1, 65) == BOOT_DIAG_OK);
    CHECK(memcmp(wire + 1, golden, 64) == 0 && wire[0] == 0xa5 && wire[65] == 0xa5);
    CHECK(boot_diag_decode(wire + 1, 65, BOOT_DIAG_PUBLISHED, &out) == BOOT_DIAG_OK);
    equal(&out, &fixture);
    memcpy(before, wire, sizeof wire);
    for (n = 0; n < 64; ++n) {
        out = fixture;
        CHECK(boot_diag_decode(wire + 1, n, BOOT_DIAG_PUBLISHED, &out) == BOOT_DIAG_ERROR_SIZE);
        equal(&out, &fixture);
        CHECK(boot_diag_encode(&fixture, BOOT_DIAG_PUBLISHED, wire + 1, n) == BOOT_DIAG_ERROR_SIZE);
        CHECK(memcmp(wire, before, sizeof wire) == 0);
    }
    CHECK(boot_diag_decode(NULL,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_ERROR_ARGUMENT);
    CHECK(boot_diag_decode(golden,64,BOOT_DIAG_PUBLISHED,NULL) == BOOT_DIAG_ERROR_ARGUMENT);
    CHECK(boot_diag_encode(NULL,BOOT_DIAG_PUBLISHED,wire,64) == BOOT_DIAG_ERROR_ARGUMENT);
    CHECK(boot_diag_encode(&fixture,BOOT_DIAG_PUBLISHED,NULL,64) == BOOT_DIAG_ERROR_ARGUMENT);
    CHECK(boot_diag_validate(NULL) == BOOT_DIAG_ERROR_ARGUMENT);
    out = fixture;
    CHECK(boot_diag_decode(golden,64,(boot_diag_validity_t)2,&out) == BOOT_DIAG_ERROR_VALIDITY);
    equal(&out, &fixture);
    CHECK(boot_diag_encode(&fixture,(boot_diag_validity_t)2,wire,64) == BOOT_DIAG_ERROR_VALIDITY);
    CHECK(memcmp(wire,before,sizeof wire) == 0);
}
static void invalid_fields(void)
{
    unsigned bit;
    const size_t offsets[] = {16,20,24,32,36,48,52,56};
    const uint32_t values[] = {6,6,21,2,2,4,8,2};
    const boot_diag_result_t errors[] = {BOOT_DIAG_ERROR_STAGE,BOOT_DIAG_ERROR_FAULT,
        BOOT_DIAG_ERROR_DETAIL,BOOT_DIAG_ERROR_SLOT,BOOT_DIAG_ERROR_SLOT,
        BOOT_DIAG_ERROR_COUNT,BOOT_DIAG_ERROR_HEALTH,BOOT_DIAG_ERROR_SLOT};
    size_t i;
    for (bit = 0; bit < 32u; ++bit) {
        malformed(0, UINT32_C(0x31444241) ^ (UINT32_C(1) << bit), BOOT_DIAG_ERROR_MAGIC);
        malformed(4, 1u ^ (UINT32_C(1) << bit), BOOT_DIAG_ERROR_VERSION);
        malformed(8, 64u ^ (UINT32_C(1) << bit), BOOT_DIAG_ERROR_LENGTH);
        malformed(60, UINT32_C(1) << bit, BOOT_DIAG_ERROR_RESERVED);
        if (bit >= 3u) malformed(52, UINT32_C(1) << bit, BOOT_DIAG_ERROR_HEALTH);
    }
    malformed(12,0,BOOT_DIAG_ERROR_VALIDITY);
    malformed(12,2,BOOT_DIAG_ERROR_VALIDITY);
    malformed(12,UINT32_MAX,BOOT_DIAG_ERROR_VALIDITY);
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        boot_diag_t record = fixture;
        uint8_t wire[64], before[64];
        malformed(offsets[i],values[i],errors[i]);
        if (errors[i] != BOOT_DIAG_ERROR_SLOT)
            malformed(offsets[i],UINT32_MAX,errors[i]);
        switch (offsets[i]) {
        case 16: record.stage = values[i]; break;
        case 20: record.fault = values[i]; break;
        case 24: record.detail = values[i]; break;
        case 32: record.running_slot = values[i]; break;
        case 36: record.selected_slot = values[i]; break;
        case 48: record.failures = values[i]; break;
        case 52: record.health = values[i]; break;
        default: record.failed_slot = values[i]; break;
        }
        memset(wire,0xa5,sizeof wire); memcpy(before,wire,sizeof wire);
        CHECK(boot_diag_encode(&record,BOOT_DIAG_PUBLISHED,wire,64) == errors[i]);
        CHECK(memcmp(wire,before,sizeof wire) == 0);
    }
}
static void legal_matrix(void)
{
    const uint32_t slots[] = {0,1,UINT32_MAX};
    const uint32_t ids[] = {0,1,UINT32_C(0x12345678),UINT32_MAX};
    unsigned stage,fault,detail,a,b,c,health,count,id,bit;
    for (stage=0; stage<=5; ++stage) for (fault=0; fault<=5; ++fault)
    for (detail=0; detail<=20; ++detail) {
        uint8_t wire[64]; boot_diag_t out, record = fixture;
        record.stage=stage; record.fault=fault; record.detail=detail;
        CHECK(boot_diag_encode(&record,BOOT_DIAG_PUBLISHED,wire,64) == BOOT_DIAG_OK);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        equal(&record,&out);
        memcpy(wire,golden,64);
        raw_word(wire,16,stage); raw_word(wire,20,fault); raw_word(wire,24,detail);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        equal(&record,&out);
    }
    for (a=0; a<3; ++a) for (b=0; b<3; ++b) for (c=0; c<3; ++c)
    for (health=0; health<=7; ++health) for (count=0; count<=3; ++count)
    for (id=0; id<4; ++id) {
        uint8_t wire[64]; boot_diag_t out, record = fixture;
        record.running_slot=slots[a]; record.selected_slot=slots[b]; record.failed_slot=slots[c];
        record.health=health; record.failures=count; record.sequence=ids[id]; record.trial_id=ids[3u-id];
        /* Independent raw fixture reaches decoder without production encoder. */
        memcpy(wire,golden,64);
        raw_word(wire,32,record.running_slot); raw_word(wire,36,record.selected_slot);
        raw_word(wire,56,record.failed_slot); raw_word(wire,52,health); raw_word(wire,48,count);
        raw_word(wire,40,record.sequence); raw_word(wire,44,record.trial_id);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        equal(&out,&record);
        CHECK(boot_diag_encode(&record,BOOT_DIAG_PUBLISHED,wire,64) == BOOT_DIAG_OK);
    }
    for (bit=0; bit<32; ++bit) {
        uint8_t wire[64]; boot_diag_t out;
        memcpy(wire,golden,64); raw_word(wire,28,UINT32_C(1)<<bit);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        CHECK(out.reset_flags == (UINT32_C(1)<<bit));
        CHECK(boot_diag_encode(&out,BOOT_DIAG_PUBLISHED,wire,64) == BOOT_DIAG_OK);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        CHECK(out.reset_flags == (UINT32_C(1)<<bit));
    }
    for (id=0; id<2; ++id) {
        uint8_t wire[64]; boot_diag_t out;
        memcpy(wire,golden,64); raw_word(wire,28,id == 0u ? 0u : UINT32_MAX);
        CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
        CHECK(out.reset_flags == (id == 0u ? 0u : UINT32_MAX));
    }
}
static void context_and_staging(void)
{
    uint8_t wire[64]; boot_diag_t record, out;
    boot_diag_context_t context, before;
    unsigned bit;
    for (bit=0; bit<32; ++bit) {
        CHECK(boot_diag_init(UINT32_C(1)<<bit,&record,&context) == BOOT_DIAG_OK);
        CHECK(record.reset_flags == (UINT32_C(1)<<bit));
    }
    CHECK(boot_diag_init(UINT32_MAX,&record,&context) == BOOT_DIAG_OK);
    CHECK(record.reset_flags == UINT32_MAX && record.stage == 0 && record.fault == 0);
    CHECK(record.detail == 0 && record.health == 0 && record.running_slot == UINT32_MAX);
    CHECK(record.selected_slot == UINT32_MAX && record.failed_slot == UINT32_MAX);
    CHECK(!context.sequence_known && !context.trial_id_known && !context.failures_known);
    CHECK(context.stage_known && context.reset_flags_known && context.health_known);
    CHECK(!boot_diag_sequence(&record,&context).available);
    CHECK(!boot_diag_trial_id(&record,&context).available);
    CHECK(!boot_diag_failures(&record,&context).available);
    context.sequence_known=true; context.trial_id_known=true; context.failures_known=true;
    CHECK(boot_diag_sequence(&record,&context).available && boot_diag_sequence(&record,&context).value == 0);
    CHECK(boot_diag_trial_id(&record,&context).available && boot_diag_trial_id(&record,&context).value == 0);
    CHECK(boot_diag_failures(&record,&context).available && boot_diag_failures(&record,&context).value == 0);
    record.sequence=UINT32_MAX; record.trial_id=UINT32_MAX;
    CHECK(boot_diag_sequence(&record,&context).value == UINT32_MAX);
    CHECK(boot_diag_trial_id(&record,&context).value == UINT32_MAX);
    context.sequence_known=false; context.trial_id_known=false;
    CHECK(!boot_diag_sequence(&record,&context).available);
    CHECK(!boot_diag_trial_id(&record,&context).available);
    CHECK(boot_diag_slot(0).available && boot_diag_slot(0).slot == 0);
    CHECK(boot_diag_slot(1).available && boot_diag_slot(1).slot == 1);
    CHECK(!boot_diag_slot(UINT32_MAX).available && !boot_diag_slot(2).available);
    CHECK(!boot_diag_sequence(NULL,&context).available && !boot_diag_sequence(&record,NULL).available);
    CHECK(!boot_diag_trial_id(NULL,&context).available && !boot_diag_trial_id(&record,NULL).available);
    CHECK(!boot_diag_failures(NULL,&context).available && !boot_diag_failures(&record,NULL).available);
    before=context; out=record;
    CHECK(boot_diag_init(0,NULL,&context) == BOOT_DIAG_ERROR_ARGUMENT);
    CHECK(memcmp(&before,&context,sizeof context) == 0);
    CHECK(boot_diag_init(0,&record,NULL) == BOOT_DIAG_ERROR_ARGUMENT); equal(&record,&out);
    /* Static fixture only: 0 -> update -> 1, no live ordering/atomicity claim. */
    CHECK(boot_diag_encode(&record,BOOT_DIAG_STAGING,wire,64) == BOOT_DIAG_OK);
    CHECK(wire[12] == 0);
    out=fixture;
    CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_ERROR_VALIDITY);
    equal(&out,&fixture);
    CHECK(boot_diag_decode(wire,64,BOOT_DIAG_STAGING,&out) == BOOT_DIAG_OK);
    record.stage=3; record.health=7;
    CHECK(boot_diag_encode(&record,BOOT_DIAG_STAGING,wire,64) == BOOT_DIAG_OK);
    raw_word(wire,12,1);
    CHECK(boot_diag_decode(wire,64,BOOT_DIAG_STAGING,&out) == BOOT_DIAG_ERROR_VALIDITY);
    CHECK(boot_diag_decode(wire,64,BOOT_DIAG_PUBLISHED,&out) == BOOT_DIAG_OK);
    equal(&record,&out);
}
int main(void)
{
    golden_and_bounds(); invalid_fields(); legal_matrix(); context_and_staging();
    printf("%u diagnostic checks passed\n",checks); return 0;
}
