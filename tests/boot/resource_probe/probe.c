/* Test-only final-link fixture. No hardware or emulator execution authorized. */
#include "lm3s6965/boot_verify.h"
#include "../verify_fixtures.h"
#include <stdint.h>

/* Observable sinks prevent dead-path elimination without touching production ABI. */
volatile boot_verify_result_t resource_probe_results[2];
volatile uint32_t resource_probe_reads;
volatile uint32_t resource_probe_init = 0x12345678u;
static boot_verified_image_t image;
typedef struct { uint32_t length; } reader_t;

static size_t read_payload(void *context, uint32_t address, uint8_t *out,
                           size_t requested)
{
    const reader_t *reader = context;
    const uint32_t offset = address - BOOT_SLOT_A_BASE;
    if (address < BOOT_SLOT_A_BASE || offset > reader->length ||
        requested > reader->length - offset || requested > BOOT_VERIFY_CHUNK_BYTES)
        return 0;
    for (size_t i = 0; i < requested; ++i)
    {
        const uint32_t pos = offset + (uint32_t)i;
        uint8_t byte = (uint8_t)(pos * 17u);
        if ((pos >= 100u && pos < 108u) || pos >= reader->length - 4u)
            byte = 0xffu;
        if (pos < 4u) byte = (uint8_t)(BOOT_INITIAL_MSP >> (8u * pos));
        else if (pos < 8u)
            byte = (uint8_t)((BOOT_SLOT_A_BASE + 9u) >> (8u * (pos - 4u)));
        out[i] = byte;
    }
    ++resource_probe_reads;
    return requested;
}

int main(void)
{
    reader_t reader = { BOOT_PAYLOAD_MIN };
    resource_probe_init ^= 1u;
    resource_probe_results[0] = boot_verify_stored(fixture_manifest[0],
        BOOT_MANIFEST_BYTES, BOOT_SLOT_A, read_payload, &reader, 0, 0, &image);
    reader.length = BOOT_PAYLOAD_MAX;
    resource_probe_results[1] = boot_verify_stored(fixture_manifest[1],
        BOOT_MANIFEST_BYTES, BOOT_SLOT_A, read_payload, &reader, 0, 0, &image);
    return (int)resource_probe_results[0] + (int)resource_probe_results[1];
}
