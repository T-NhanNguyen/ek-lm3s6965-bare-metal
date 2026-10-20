#define _XOPEN_SOURCE 700
#include "fixture_metadata.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int release_number(const char *text, uint32_t *out)
{
    char *end;
    if (*text < '0' || *text > '9') return 0;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

static int verify_file(const char *path, uint32_t slot, uint32_t release,
                       boot_verified_image_t *image)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = malloc(BOOT_PACKAGE_MAX + 1u);
    if (!file || !bytes) {
        if (file) fclose(file);
        free(bytes);
        fprintf(stderr, "cannot read package: %s\n", path);
        return 0;
    }
    size_t length = fread(bytes, 1, BOOT_PACKAGE_MAX + 1u, file);
    int readable = !ferror(file);
    if (fclose(file) != 0) readable = 0;
    int ok = readable && fixture_verify_package(bytes, length, slot, release, image);
    free(bytes);
    return ok;
}

static int write_pages(const char *directory, const uint8_t *page0,
                       const uint8_t *page1)
{
    /* Resolve symlinks before rejecting private-vault destinations. Caller
     * supplies an existing ignored output directory, never a flash device. */
    char *dir = realpath(directory, NULL);
    if (!dir) return 0;
    size_t n = strlen(dir);
    if (strstr(dir, "/.dev-vault/") ||
        (n >= 11u && strcmp(dir + n - 11u, "/.dev-vault") == 0)) {
        free(dir);
        return 0;
    }
    char *paths[2] = {malloc(n + 32u), malloc(n + 32u)};
    if (!paths[0] || !paths[1]) {
        free(paths[0]); free(paths[1]); free(dir);
        return 0;
    }
    snprintf(paths[0], n + 32u, "%s/metadata-page0.bin", dir);
    snprintf(paths[1], n + 32u, "%s/metadata-page1.bin", dir);
    free(dir);
    /* Exclusive creation: retained fixture/artifact files are never replaced.
     * Both names must be available before writing either complete page. */
    FILE *first = fopen(paths[0], "wbx");
    FILE *second = first ? fopen(paths[1], "wbx") : NULL;
    const int created_first = first != NULL;
    const int created_second = second != NULL;
    int ok = created_first && created_second;
    if (ok) {
        ok = fwrite(page0, 1, BOOT_ERASE_BYTES, first) == BOOT_ERASE_BYTES;
        if (fwrite(page1, 1, BOOT_ERASE_BYTES, second) != BOOT_ERASE_BYTES) ok = 0;
    }
    if (first && fclose(first) != 0) ok = 0;
    if (second && fclose(second) != 0) ok = 0;
    if (!ok) {
        if (created_first) remove(paths[0]);
        if (created_second) remove(paths[1]);
    }
    free(paths[0]); free(paths[1]);
    return ok;
}

int main(int argc, char **argv)
{
    const char *a = NULL, *b = NULL, *dir = NULL;
    uint32_t releases[2] = {0};
    int have_a = 0, have_b = 0, verify_only = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--verify-only") && !verify_only) {
            verify_only = 1;
            continue;
        }
        if (i + 1 >= argc) goto usage;
        const char *value = argv[i + 1];
        if (!strcmp(argv[i], "--package-a") && !a) a = value;
        else if (!strcmp(argv[i], "--package-b") && !b) b = value;
        else if (!strcmp(argv[i], "--release-a") && !have_a) {
            if (!release_number(value, &releases[0])) goto usage;
            have_a = 1;
        } else if (!strcmp(argv[i], "--release-b") && !have_b) {
            if (!release_number(value, &releases[1])) goto usage;
            have_b = 1;
        } else if (!strcmp(argv[i], "--output-dir") && !dir) dir = value;
        else goto usage;
        ++i;
    }
    if (!a || !b || !have_a || !have_b || (verify_only ? dir != NULL : dir == NULL))
        goto usage;
    boot_verified_image_t images[2];
    uint8_t page0[BOOT_ERASE_BYTES], page1[BOOT_ERASE_BYTES];
    if (!verify_file(a, BOOT_SLOT_A, releases[0], &images[0]) ||
        !verify_file(b, BOOT_SLOT_B, releases[1], &images[1])) return 1;
    if (!fixture_metadata_pages(images, page0, page1)) return 1;
    if (!verify_only && !write_pages(dir, page0, page1)) {
        fprintf(stderr, "cannot exclusively create metadata pages in output directory\n");
        return 1;
    }
    printf("verified A release %lu bytes %lu; B release %lu bytes %lu; "
           "metadata A=current B=previous confirmed=3 sequence=1%s\n",
        (unsigned long)releases[0], (unsigned long)images[0].manifest.payload_length,
        (unsigned long)releases[1], (unsigned long)images[1].manifest.payload_length,
        verify_only ? " (verify only; no output)" : " (page0 valid; page1 erased)");
    return 0;
usage:
    fprintf(stderr, "usage: %s --package-a FILE --release-a DECIMAL "
        "--package-b FILE --release-b DECIMAL "
        "(--output-dir EXISTING_OUTPUT_DIR | --verify-only)\n", argv[0]);
    return 2;
}
