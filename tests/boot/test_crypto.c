#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "boot_demo_public_key.h"

/* Independent public known-answer fixture: RFC 6979 A.2.5, P-256,
 * SHA-256, message "sample". Only public Q, digest, and r/s are retained;
 * no signing operation, nonce or private key is used by this test.
 * https://www.rfc-editor.org/rfc/rfc6979#appendix-A.2.5
 * This is NOT a demo-key signature or a signed firmware manifest.
 */
static const char kat_public_hex[] =
    "04"
    "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
    "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299";
static const char kat_digest_hex[] =
    "AF2BDBE1AA9B6EC1E2ADE1D694F41FC71A831D0268E9891562113D8A62ADD1BF";
static const char kat_signature_hex[] =
    "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"
    "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8";
static const char order_hex[] =
    "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";

static unsigned int checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static unsigned int hex_digit(char c)
{
    if (c >= '0' && c <= '9') { return (unsigned int)(c - '0'); }
    if (c >= 'A' && c <= 'F') { return (unsigned int)(c - 'A') + 10u; }
    if (c >= 'a' && c <= 'f') { return (unsigned int)(c - 'a') + 10u; }
    CHECK(0);
    return 0u;
}

static void decode_hex(const char *hex, uint8_t *out, size_t length)
{
    CHECK(strlen(hex) == length * 2u);
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)((hex_digit(hex[i * 2u]) << 4) | hex_digit(hex[i * 2u + 1u]));
    }
}

/* Test-only API exercise, not a boot verifier. Takes a digest, never a message.
 * Point import alone is insufficient: check_pubkey MUST precede verify.
 * Upstream ecdsa_verify enforces 1 <= r,s < n (also tested below).
 */
static int verify_digest(const uint8_t public_key[65], const uint8_t digest[32],
                         const uint8_t signature[64])
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r, s;
    int result;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) { result = mbedtls_ecp_point_read_binary(&group, &point, public_key, 65u); }
    if (result == 0) { result = mbedtls_ecp_check_pubkey(&group, &point); }
    if (result == 0) { result = mbedtls_mpi_read_binary(&r, signature, 32u); }
    if (result == 0) { result = mbedtls_mpi_read_binary(&s, signature + 32u, 32u); }
    if (result == 0) { result = mbedtls_ecdsa_verify(&group, digest, 32u, &point, &r, &s); }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return result;
}

static void test_sha256(void)
{
    static const uint8_t message[] = "sample";
    static const uint8_t abc[] = "abc";
    uint8_t digest[32], expected[32];
    mbedtls_sha256_context context;
    decode_hex(kat_digest_hex, expected, sizeof(expected));
    CHECK(mbedtls_sha256(message, sizeof(message) - 1u, digest, 0) == 0);
    CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
    mbedtls_sha256_init(&context);
    CHECK(mbedtls_sha256_starts(&context, 0) == 0);
    CHECK(mbedtls_sha256_update(&context, message, 2u) == 0);
    CHECK(mbedtls_sha256_update(&context, message + 2u, sizeof(message) - 3u) == 0);
    CHECK(mbedtls_sha256_finish(&context, digest) == 0);
    CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
    mbedtls_sha256_free(&context);
    decode_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, 32u);
    CHECK(mbedtls_sha256(message, 0u, digest, 0) == 0);
    CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
    decode_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, 32u);
    CHECK(mbedtls_sha256(abc, sizeof(abc) - 1u, digest, 0) == 0);
    CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
}

static void test_ecdsa(void)
{
    uint8_t public_key[65], digest[32], signature[64];
    uint8_t changed_digest[32], changed_signature[64], changed_public[65];
    decode_hex(kat_public_hex, public_key, sizeof(public_key));
    decode_hex(kat_digest_hex, digest, sizeof(digest));
    decode_hex(kat_signature_hex, signature, sizeof(signature));
    CHECK(verify_digest(public_key, digest, signature) == 0);
    memcpy(changed_digest, digest, sizeof(digest));
    changed_digest[0] ^= 1u;
    CHECK(verify_digest(public_key, changed_digest, signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
    CHECK(mbedtls_sha256(digest, sizeof(digest), changed_digest, 0) == 0);
    /* SHA256(SHA256(message)) must NOT verify: protects digest-input contract. */
    CHECK(verify_digest(public_key, changed_digest, signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
    memcpy(changed_signature, signature, sizeof(signature));
    changed_signature[31] ^= 1u;
    CHECK(verify_digest(public_key, digest, changed_signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
    memcpy(changed_signature, signature, sizeof(signature));
    changed_signature[63] ^= 1u;
    CHECK(verify_digest(public_key, digest, changed_signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
    for (size_t offset = 0u; offset <= 32u; offset += 32u) {
        memcpy(changed_signature, signature, sizeof(signature));
        memset(changed_signature + offset, 0, 32u);
        CHECK(verify_digest(public_key, digest, changed_signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
        decode_hex(order_hex, changed_signature + offset, 32u);
        CHECK(verify_digest(public_key, digest, changed_signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
        memset(changed_signature + offset, 0xff, 32u);
        CHECK(verify_digest(public_key, digest, changed_signature) == MBEDTLS_ERR_ECP_VERIFY_FAILED);
    }
    memcpy(changed_public, public_key, sizeof(public_key));
    changed_public[64] ^= 1u;
    CHECK(verify_digest(changed_public, digest, signature) == MBEDTLS_ERR_ECP_INVALID_KEY);
    memset(changed_public, 0, sizeof(changed_public));
    changed_public[0] = 4u; /* affine (0,0), not on P-256 */
    CHECK(verify_digest(changed_public, digest, signature) == MBEDTLS_ERR_ECP_INVALID_KEY);
    memset(changed_public + 1u, 0xff, 32u); /* x outside field */
    CHECK(verify_digest(changed_public, digest, signature) == MBEDTLS_ERR_ECP_INVALID_KEY);
    changed_public[0] = 5u; /* invalid SEC1 tag */
    CHECK(verify_digest(changed_public, digest, signature) != 0);
    /* Group closure: no other curves have been enabled. */
    mbedtls_ecp_group group;
    mbedtls_ecp_group_init(&group);
    CHECK(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP384R1) == MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE);
    mbedtls_ecp_group_free(&group);
}

static void test_demo_public(void)
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    uint8_t digest[32], expected[32];
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    CHECK(sizeof(lm3s6965_boot_demo_public_key) == 65u);
    CHECK(lm3s6965_boot_demo_public_key[0] == 4u);
    CHECK(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0);
    CHECK(mbedtls_ecp_point_read_binary(&group, &point, lm3s6965_boot_demo_public_key, 65u) == 0);
    CHECK(mbedtls_ecp_check_pubkey(&group, &point) == 0);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    decode_hex("f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d", expected, 32u);
    CHECK(mbedtls_sha256(lm3s6965_boot_demo_public_key, 65u, digest, 0) == 0);
    CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
}

int main(void)
{
    test_sha256();
    test_ecdsa();
    test_demo_public();
    printf("boot crypto: %u checks PASS\n", checks);
    return EXIT_SUCCESS;
}
