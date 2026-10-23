/* Native test glue only: exact transport length before reviewed C verifier. */
#include "lm3s6965/boot_verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const uint8_t *payload;
    size_t length;
    uint32_t base;
} package_reader_t;

static size_t read_payload(void *context, uint32_t address, uint8_t *buffer,
                           size_t requested)
{
    const package_reader_t *reader = context;
    size_t offset;
    if (address < reader->base) { return 0; }
    offset = (size_t)(address - reader->base);
    if (offset > reader->length || requested > reader->length - offset) { return 0; }
    memcpy(buffer, reader->payload + offset, requested);
    return requested;
}

int main(int argc, char **argv)
{
    uint8_t wire[BOOT_PACKAGE_MAX + 1u];
    size_t length;
    FILE *file;
    uint32_t slot;
    boot_manifest_t manifest;
    boot_verified_image_t verified;
    boot_verify_result_t result;
    package_reader_t reader;
    if (argc != 3 || (strcmp(argv[1], "A") != 0 && strcmp(argv[1], "B") != 0))
    {
        fprintf(stderr, "usage: package_verify A|B package\n");
        return 2;
    }
    slot = strcmp(argv[1], "A") == 0 ? BOOT_SLOT_A : BOOT_SLOT_B;
    file = fopen(argv[2], "rb");
    if (file == NULL) { return 2; }
    length = fread(wire, 1u, sizeof(wire), file);
    if (ferror(file) != 0) { (void)fclose(file); return 2; }
    (void)fclose(file);
    if (length > BOOT_PACKAGE_MAX ||
        boot_manifest_decode(wire, length, &manifest) != BOOT_OK ||
        length != BOOT_MANIFEST_BYTES + (size_t)manifest.payload_length)
    {
        fprintf(stderr, "package length/format rejected\n");
        return 1;
    }
    reader.payload = wire + BOOT_MANIFEST_BYTES;
    reader.length = manifest.payload_length;
    reader.base = slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    result = boot_verify_stored(wire, BOOT_MANIFEST_BYTES, slot, read_payload,
                               &reader, NULL, NULL, &verified);
    if (result != BOOT_VERIFY_OK)
    {
        fprintf(stderr, "verification rejected: %u\n", (unsigned)result);
        return 1;
    }
    printf("verified slot %s release %lu payload %lu\n", argv[1],
           (unsigned long)verified.manifest.release,
           (unsigned long)verified.manifest.payload_length);
    return 0;
}
