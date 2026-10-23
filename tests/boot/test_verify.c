#include "lm3s6965/boot_verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "verify_fixtures.h"

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(EXIT_FAILURE); \
} } while (0)

#ifdef BOOT_VERIFY_FAULT_INJECTION
/* Only the isolated host crypto copy calls these; no runtime API/test hooks. */
void *boot_verify_test_calloc(size_t count, size_t size);
void boot_verify_test_free(void *pointer);
static size_t allocation_calls, fail_at, live_allocations;
void *boot_verify_test_calloc(size_t count, size_t size)
{
    void *pointer;
    ++allocation_calls;
    if (fail_at != 0u && allocation_calls == fail_at)
        return NULL;
    pointer = calloc(count, size);
    if (pointer != NULL) ++live_allocations;
    return pointer;
}
void boot_verify_test_free(void *pointer)
{
    if (pointer != NULL)
    {
        CHECK(live_allocations > 0u);
        --live_allocations;
    }
    free(pointer);
}
#endif

static uint8_t payload[BOOT_PAYLOAD_MAX];
typedef struct
{
    uint32_t base, length, completed;
    unsigned reads, events;
    unsigned bad_read; /* 1 short, 2 zero, 3 oversize */
    uint32_t bad_read_after; /* 0 = first; otherwise fail at this completed count */
    int abort_phase;
    uint32_t abort_after;
} stream_t;

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i)
        p[i] = (uint8_t)(value >> (8u * i));
}

static stream_t prepare(unsigned slot, unsigned mode)
{
    stream_t stream = {0};
    uint32_t reset;
    stream.base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    stream.length = mode == 1u ? BOOT_PAYLOAD_MAX : BOOT_PAYLOAD_MIN;
    stream.abort_phase = -1;
    for (uint32_t i = 0u; i < stream.length; ++i)
        payload[i] = (uint8_t)(i * 17u + slot * 31u);
    memset(payload + 100u, 0xff, 8u);
    memset(payload + stream.length - 4u, 0xff, 4u);
    put32(payload, mode == 2u ? UINT32_C(0x2000fffc) : BOOT_INITIAL_MSP);
    reset = stream.base + 9u;
    if (mode == 3u) reset = stream.base + 8u;
    if (mode == 4u) reset = stream.base - 1u;
    if (mode == 5u) reset = stream.base + stream.length + 1u;
    if (mode == 6u) reset = stream.base + stream.length - 1u;
    put32(payload + 4u, reset);
    return stream;
}

static size_t read_payload(void *context, uint32_t address, uint8_t *out,
                           size_t requested)
{
    stream_t *stream = context;
    CHECK(requested > 0u && requested <= BOOT_VERIFY_CHUNK_BYTES);
    CHECK(address == stream->base + stream->completed);
    CHECK(address >= stream->base && address < stream->base + stream->length);
    CHECK(requested <= stream->length - stream->completed);
    memcpy(out, payload + stream->completed, requested);
    stream->completed += (uint32_t)requested;
    ++stream->reads;
    if (stream->completed >= stream->bad_read_after)
    {
        if (stream->bad_read == 1u) return requested - 1u;
        if (stream->bad_read == 2u) return 0u;
        if (stream->bad_read == 3u) return requested + 1u;
    }
    return requested;
}

static int progress(void *context, boot_verify_phase_t phase,
                    uint32_t completed, uint32_t total)
{
    stream_t *stream = context;
    CHECK(total == stream->length);
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN)
        CHECK(stream->events == 0u && stream->reads == 0u && completed == 0u);
    else if (phase == BOOT_VERIFY_MANIFEST_AUTHENTICATED)
        CHECK(stream->events == 1u && stream->reads == 0u && completed == 0u);
    else
    {
        CHECK(phase == BOOT_VERIFY_PAYLOAD_PROGRESS);
        CHECK(completed == stream->completed && completed <= total);
        CHECK(stream->events == stream->reads + 1u);
    }
    ++stream->events;
    return (int)phase != stream->abort_phase || completed < stream->abort_after;
}

static boot_verify_result_t verify(const uint8_t *wire, size_t size,
                                   unsigned slot, stream_t *stream,
                                   boot_verified_image_t *image)
{
    return boot_verify_stored(wire, size, slot, read_payload, stream,
                              progress, stream, image);
}

static void expect_failure(const uint8_t *wire, size_t size, unsigned slot,
                           stream_t *stream, boot_verify_result_t expected)
{
    boot_verified_image_t image, before;
    memset(&image, 0xa5, sizeof image);
    memcpy(&before, &image, sizeof before);
    CHECK(verify(wire, size, slot, stream, &image) == expected);
    CHECK(memcmp(&image, &before, sizeof image) == 0);
}

static void test_fixtures(void)
{
    static const boot_verify_result_t results[7] = {
        BOOT_VERIFY_OK, BOOT_VERIFY_OK, BOOT_VERIFY_ERROR_MSP,
        BOOT_VERIFY_ERROR_THUMB, BOOT_VERIFY_ERROR_VECTOR_RANGE,
        BOOT_VERIFY_ERROR_VECTOR_RANGE, BOOT_VERIFY_OK
    };
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        for (unsigned mode = 0u; mode < 7u; ++mode)
        {
            unsigned index = slot * 7u + mode;
            stream_t stream = prepare(slot, mode);
            boot_manifest_t manifest;
            boot_verified_image_t image;
            CHECK(boot_verify_manifest(fixture_manifest[index], 160u, slot,
                                       NULL, NULL, &manifest) == BOOT_VERIFY_OK);
            CHECK(manifest.release == UINT32_MAX);
            if (results[mode] == BOOT_VERIFY_OK)
            {
                CHECK(verify(fixture_manifest[index], 160u, slot, &stream, &image)
                      == BOOT_VERIFY_OK);
                CHECK(image.manifest.slot == slot);
                CHECK(image.manifest.execution_base == stream.base);
                CHECK(image.manifest.payload_length == stream.length);
                CHECK(memcmp(image.manifest.payload_hash, fixture_manifest[index] + 48u, 32u) == 0);
                CHECK(image.initial_msp == BOOT_INITIAL_MSP);
                CHECK(image.reset_vector == stream.base +
                      (mode == 6u ? stream.length - 1u : 9u));
            }
            else
                expect_failure(fixture_manifest[index], 160u, slot, &stream, results[mode]);
            CHECK(stream.completed == stream.length);
            CHECK(stream.reads == (stream.length + 255u) / 256u);
        }
    }
}

static void test_admission(void)
{
    uint8_t wire[1024];
    boot_manifest_t manifest, before;
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        /* Every active signed byte: structural gates OR signature failure.
         * Zero reader calls throughout; malformed fields also zero BEGIN. */
        for (size_t offset = 0u; offset < 160u; ++offset)
        {
            stream_t stream = prepare(slot, 0u);
            boot_manifest_t parsed;
            memcpy(wire, fixture_manifest[slot * 7u], 160u);
            wire[offset] ^= 1u;
            boot_verify_result_t expected =
                boot_manifest_decode(wire, 160u, &parsed) == BOOT_OK ?
                BOOT_VERIFY_ERROR_SIGNATURE : BOOT_VERIFY_ERROR_FORMAT;
            if (expected == BOOT_VERIFY_ERROR_SIGNATURE)
                stream.length = parsed.payload_length;
            expect_failure(wire, 160u, slot, &stream, expected);
            CHECK(stream.reads == 0u);
            CHECK(stream.events == (expected == BOOT_VERIFY_ERROR_FORMAT ? 0u : 1u));
            memset(&manifest, 0x5a, sizeof manifest);
            memcpy(&before, &manifest, sizeof before);
            CHECK(boot_verify_manifest(wire, 160u, slot, NULL, NULL, &manifest) == expected);
            CHECK(memcmp(&manifest, &before, sizeof manifest) == 0);
        }
        for (size_t size = 0u; size < 160u; ++size)
        {
            stream_t stream = prepare(slot, 0u);
            expect_failure(fixture_manifest[slot * 7u], size, slot, &stream,
                           BOOT_VERIFY_ERROR_FORMAT);
            CHECK(stream.reads == 0u && stream.events == 0u);
        }
        {
            stream_t stream = prepare(slot, 0u);
            expect_failure(fixture_manifest[(1u - slot) * 7u], 160u, slot,
                           &stream, BOOT_VERIFY_ERROR_TARGET);
            CHECK(stream.reads == 0u && stream.events == 0u);
        }
        /* Tail deliberately ignored and never read, not transported. */
        memset(wire, 0x34, sizeof wire);
        memcpy(wire, fixture_manifest[slot * 7u], 160u);
        CHECK(boot_verify_manifest(wire, sizeof wire, slot, NULL, NULL, &manifest) == BOOT_VERIFY_OK);
        static const uint32_t lengths[] = {0u, 232u, 235u, 237u, 111615u,
            111617u, 111620u, UINT32_MAX, UINT32_C(0xfffffff0)};
        for (size_t i = 0u; i < sizeof lengths / sizeof lengths[0]; ++i)
        {
            stream_t stream = prepare(slot, 0u);
            memcpy(wire, fixture_manifest[slot * 7u], 160u);
            put32(wire + 36u, lengths[i]);
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_FORMAT);
            CHECK(stream.reads == 0u && stream.events == 0u);
        }
        static const size_t fields[] = {16u, 20u, 24u, 28u, 32u, 44u};
        for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; ++i)
        {
            stream_t stream = prepare(slot, 0u);
            memcpy(wire, fixture_manifest[slot * 7u], 160u);
            put32(wire + fields[i], UINT32_MAX);
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_FORMAT);
            CHECK(stream.reads == 0u && stream.events == 0u);
        }
        /* Valid structure with release=0 is still signed, no metadata shortcut. */
        {
            stream_t stream = prepare(slot, 0u);
            memcpy(wire, fixture_manifest[slot * 7u], 160u);
            put32(wire + 40u, 0u);
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_SIGNATURE);
            CHECK(stream.reads == 0u && stream.events == 1u);
        }
    }
}

static void test_scalars(void)
{
    static const uint8_t order[32] = {
        0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,
        0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51
    };
    uint8_t wire[160];
    for (unsigned slot = 0u; slot < 2u; ++slot)
        for (size_t offset = 96u; offset <= 128u; offset += 32u)
            for (unsigned mode = 0u; mode < 5u; ++mode)
            {
                stream_t stream = prepare(slot, 0u);
                memcpy(wire, fixture_manifest[slot * 7u], sizeof wire);
                memset(wire + offset, mode == 2u ? 0xff : 0, 32u);
                if (mode == 1u) memcpy(wire + offset, order, 32u);
                if (mode == 3u) wire[offset + 31u] = 1u; /* in-range invalid signature */
                if (mode == 4u)
                {
                    memcpy(wire + offset, order, 32u);
                    --wire[offset + 31u]; /* n-1 is admitted scalar, bad signature */
                }
                expect_failure(wire, sizeof wire, slot, &stream, BOOT_VERIFY_ERROR_SIGNATURE);
                CHECK(stream.reads == 0u && stream.events == 1u);
            }
}

static void test_stream_failures(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        const uint8_t *wire = fixture_manifest[slot * 7u + 1u];
        /* Corrupt vectors/gap/final pad/code: all fail hash before vectors. */
        static const uint32_t offsets[] = {0u, 4u, 100u, 200u, BOOT_PAYLOAD_MAX - 1u};
        for (size_t i = 0u; i < sizeof offsets / sizeof offsets[0]; ++i)
        {
            stream_t stream = prepare(slot, 1u);
            payload[offsets[i]] ^= 1u;
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_HASH);
            CHECK(stream.completed == stream.length);
        }
        for (unsigned mode = 1u; mode <= 3u; ++mode)
        {
            stream_t stream = prepare(slot, 1u);
            stream.bad_read = mode;
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_READ);
            CHECK(stream.reads == 1u && stream.events == 2u);
        }
        for (unsigned boundary = 0u; boundary < 2u; ++boundary)
        {
            stream_t stream = prepare(slot, 1u);
            stream.bad_read = 1u;
            stream.bad_read_after = boundary == 0u ? 512u : stream.length;
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_READ);
            CHECK(stream.completed == stream.bad_read_after);
            CHECK(stream.events == stream.reads + 1u);
        }
        for (int phase = 0; phase <= 2; ++phase)
        {
            stream_t stream = prepare(slot, 1u);
            stream.abort_phase = phase;
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_ABORTED);
            CHECK(stream.reads == (phase == 2 ? 1u : 0u));
        }
        for (unsigned boundary = 0u; boundary < 2u; ++boundary)
        {
            stream_t stream = prepare(slot, 1u);
            stream.abort_phase = (int)BOOT_VERIFY_PAYLOAD_PROGRESS;
            stream.abort_after = boundary == 0u ? 512u : stream.length;
            expect_failure(wire, 160u, slot, &stream, BOOT_VERIFY_ERROR_ABORTED);
            CHECK(stream.completed == stream.abort_after);
        }
        /* Optional callbacks, no reader or output pointers borrowed after use. */
        {
            stream_t stream = prepare(slot, 1u);
            boot_verified_image_t image;
            CHECK(boot_verify_stored(wire, 160u, slot, read_payload, &stream,
                                     NULL, NULL, &image) == BOOT_VERIFY_OK);
            CHECK(stream.completed == stream.length);
        }
    }
}

static void test_arguments(void)
{
    boot_verified_image_t image, before;
    boot_manifest_t manifest, old_manifest;
    stream_t stream = prepare(0u, 0u);
    const uint8_t *wire = fixture_manifest[0];
    memset(&image, 0xab, sizeof image);
    memcpy(&before, &image, sizeof image);
    memset(&manifest, 0xcd, sizeof manifest);
    memcpy(&old_manifest, &manifest, sizeof manifest);
    CHECK(boot_verify_manifest(NULL, 160u, 0u, NULL, NULL, &manifest) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(boot_verify_manifest(wire, 160u, 2u, NULL, NULL, &manifest) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(boot_verify_manifest(wire, 160u, BOOT_SLOT_NONE, NULL, NULL, &manifest) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(boot_verify_manifest(wire, 160u, 0u, NULL, NULL, NULL) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(memcmp(&manifest, &old_manifest, sizeof manifest) == 0);
    CHECK(boot_verify_stored(wire, 160u, 0u, NULL, NULL, progress, &stream, &image) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(boot_verify_stored(wire, 160u, 0u, read_payload, &stream, progress, &stream, NULL) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(verify(NULL, 160u, 0u, &stream, &image) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(verify(wire, 160u, 2u, &stream, &image) == BOOT_VERIFY_ERROR_ARGUMENT);
    CHECK(memcmp(&image, &before, sizeof image) == 0);
    CHECK(stream.reads == 0u && stream.events == 0u);
}

#ifdef BOOT_VERIFY_FAULT_INJECTION
static void test_allocation_failures(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        boot_verified_image_t image;
        stream_t stream = prepare(slot, 0u);
        allocation_calls = 0u;
        fail_at = 0u;
        CHECK(verify(fixture_manifest[slot * 7u], 160u, slot, &stream, &image) == BOOT_VERIFY_OK);
        CHECK(live_allocations == 0u);
        size_t calls = allocation_calls;
        CHECK(calls > 0u);
        printf("boot verify fault: slot %u, %zu allocations in success path\n", slot, calls);
        fflush(stdout);
        /* Bounded sampling: first/last 16 calls plus 64 evenly spaced calls.
         * P-256 has tens of thousands of repeated arithmetic allocations;
         * exhaustive per-call failure is deliberately NOT a test claim.
         * Each sampled failure must release every successful allocation. */
        for (size_t sample = 0u; sample < 96u; ++sample)
        {
            size_t failure = sample < 16u ? sample + 1u :
                (sample < 32u ? calls - (sample - 16u) :
                 1u + ((calls - 1u) * (sample - 32u)) / 63u);
            stream = prepare(slot, 0u);
            allocation_calls = 0u;
            fail_at = failure;
            expect_failure(fixture_manifest[slot * 7u], 160u, slot, &stream,
                           BOOT_VERIFY_ERROR_CRYPTO);
            CHECK(live_allocations == 0u);
            CHECK(stream.reads == 0u);
        }
        fail_at = 0u;
        printf("boot verify fault: slot %u, 96 sampled allocation failure cases PASS\n", slot);
    }
}
#endif

int main(void)
{
    test_fixtures();
    test_admission();
    test_scalars();
    test_stream_failures();
    test_arguments();
#ifdef BOOT_VERIFY_FAULT_INJECTION
    test_allocation_failures();
    CHECK(live_allocations == 0u);
#endif
    printf("boot verify: %u checks PASS\n", checks);
    return EXIT_SUCCESS;
}
