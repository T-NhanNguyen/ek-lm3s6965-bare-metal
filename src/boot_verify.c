#include "lm3s6965/boot_verify.h"

#include <string.h>
#include "boot_demo_public_key.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

static int notify(boot_verify_progress_fn progress, void *context,
                  boot_verify_phase_t phase, uint32_t completed, uint32_t total)
{
    return progress == NULL || progress(context, phase, completed, total) != 0;
}

boot_verify_result_t boot_verify_manifest(const uint8_t *wire, size_t size,
    uint32_t expected_slot, boot_verify_progress_fn progress, void *context,
    boot_manifest_t *out)
{
    uint8_t prefix[BOOT_MANIFEST_SIGNED_BYTES], digest[32];
    boot_manifest_t manifest;
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r, s;
    int error;
    boot_verify_result_t result = BOOT_VERIFY_ERROR_CRYPTO;
    if (wire == NULL || out == NULL ||
        (expected_slot != BOOT_SLOT_A && expected_slot != BOOT_SLOT_B))
        return BOOT_VERIFY_ERROR_ARGUMENT;
    if (boot_manifest_decode(wire, size, &manifest) != BOOT_OK)
        return BOOT_VERIFY_ERROR_FORMAT;
    if (manifest.slot != expected_slot)
        return BOOT_VERIFY_ERROR_TARGET;
    /* Snapshot before calling user code; signature/hash refer to same bytes. */
    memcpy(prefix, wire, sizeof prefix);
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    if (!notify(progress, context, BOOT_VERIFY_MANIFEST_BEGIN, 0u,
                manifest.payload_length))
    {
        result = BOOT_VERIFY_ERROR_ABORTED;
        goto cleanup;
    }
    error = mbedtls_sha256(prefix, sizeof prefix, digest, 0);
    if (error == 0)
        error = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (error == 0)
        error = mbedtls_ecp_point_read_binary(&group, &point,
                    lm3s6965_boot_demo_public_key,
                    sizeof lm3s6965_boot_demo_public_key);
    if (error == 0)
        error = mbedtls_ecp_check_pubkey(&group, &point);
    if (error == 0)
        error = mbedtls_mpi_read_binary(&r, manifest.signature, 32u);
    if (error == 0)
        error = mbedtls_mpi_read_binary(&s, manifest.signature + 32u, 32u);
    /* mbedtls_ecdsa_verify enforces 1 <= r,s < P-256 n. */
    if (error == 0)
        error = mbedtls_ecdsa_verify(&group, digest, sizeof digest, &point, &r, &s);
    if (error != 0)
    {
        if (error == MBEDTLS_ERR_ECP_VERIFY_FAILED)
            result = BOOT_VERIFY_ERROR_SIGNATURE;
        goto cleanup;
    }
    if (!notify(progress, context, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0u,
                manifest.payload_length))
    {
        result = BOOT_VERIFY_ERROR_ABORTED;
        goto cleanup;
    }
    *out = manifest;
    result = BOOT_VERIFY_OK;
cleanup:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    mbedtls_platform_zeroize(prefix, sizeof prefix);
    mbedtls_platform_zeroize(digest, sizeof digest);
    mbedtls_platform_zeroize(&manifest, sizeof manifest);
    return result;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

boot_verify_result_t boot_verify_stored(const uint8_t *wire, size_t size,
    uint32_t expected_slot, boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_verified_image_t *out)
{
    boot_verified_image_t image;
    uint8_t buffer[BOOT_VERIFY_CHUNK_BYTES], vectors[8], digest[32];
    uint32_t completed = 0u, end, target;
    mbedtls_sha256_context sha;
    boot_verify_result_t result;
    if (reader == NULL || out == NULL)
        return BOOT_VERIFY_ERROR_ARGUMENT;
    result = boot_verify_manifest(wire, size, expected_slot, progress,
                                  progress_context, &image.manifest);
    if (result != BOOT_VERIFY_OK)
        return result;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0)
    {
        result = BOOT_VERIFY_ERROR_CRYPTO;
        goto cleanup;
    }
    while (completed < image.manifest.payload_length)
    {
        uint32_t remaining = image.manifest.payload_length - completed;
        size_t requested = remaining > BOOT_VERIFY_CHUNK_BYTES ?
                           BOOT_VERIFY_CHUNK_BYTES : (size_t)remaining;
        if (reader(reader_context, image.manifest.execution_base + completed,
                   buffer, requested) != requested)
        {
            result = BOOT_VERIFY_ERROR_READ;
            goto cleanup;
        }
        if (completed == 0u)
            memcpy(vectors, buffer, sizeof vectors);
        if (mbedtls_sha256_update(&sha, buffer, requested) != 0)
        {
            result = BOOT_VERIFY_ERROR_CRYPTO;
            goto cleanup;
        }
        completed += (uint32_t)requested;
        if (!notify(progress, progress_context, BOOT_VERIFY_PAYLOAD_PROGRESS,
                    completed, image.manifest.payload_length))
        {
            result = BOOT_VERIFY_ERROR_ABORTED;
            goto cleanup;
        }
    }
    if (mbedtls_sha256_finish(&sha, digest) != 0)
    {
        result = BOOT_VERIFY_ERROR_CRYPTO;
        goto cleanup;
    }
    if (memcmp(digest, image.manifest.payload_hash, sizeof digest) != 0)
    {
        result = BOOT_VERIFY_ERROR_HASH;
        goto cleanup;
    }
    image.initial_msp = get32(vectors);
    image.reset_vector = get32(vectors + 4);
    if (image.initial_msp != BOOT_INITIAL_MSP)
        result = BOOT_VERIFY_ERROR_MSP;
    else if ((image.reset_vector & 1u) == 0u)
        result = BOOT_VERIFY_ERROR_THUMB;
    else
    {
        /* Already bounded before crypto/read; no untrusted addition wraps. */
        end = image.manifest.execution_base + image.manifest.payload_length;
        target = image.reset_vector & ~UINT32_C(1);
        result = target < image.manifest.execution_base || target >= end ?
                 BOOT_VERIFY_ERROR_VECTOR_RANGE : BOOT_VERIFY_OK;
    }
    if (result == BOOT_VERIFY_OK)
        *out = image;
cleanup:
    mbedtls_sha256_free(&sha);
    mbedtls_platform_zeroize(buffer, sizeof buffer);
    mbedtls_platform_zeroize(vectors, sizeof vectors);
    mbedtls_platform_zeroize(digest, sizeof digest);
    mbedtls_platform_zeroize(&image, sizeof image);
    return result;
}
