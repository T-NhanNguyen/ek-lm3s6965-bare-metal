#ifndef LM3S6965_MBEDTLS_BOOT_H
#define LM3S6965_MBEDTLS_BOOT_H

/* Local Mbed TLS 3.6.7 configuration, applied to the library AND consumers.
 * Raw digest-input P-256 verification and SHA-256 only; no TLS, PSA, PK,
 * certificates, entropy, DRBG or deterministic signing dependencies.
 * ECDSA_C itself also compiles signing/DER APIs in upstream 3.6.7; there is
 * no verification-only switch. Section GC can discard unused APIs at link.
 * Both ASN.1 switches are required by upstream check_config.h even for raw
 * r/s verification. Do not include the upstream default configuration.
 * MPI uses the default calloc/free allocator: target heap budgeting is future
 * work, not established by this static-library increment.
 */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
#define MBEDTLS_SHA256_C

/* Reduce ROM at the cost of runtime: omit fixed-generator precomputation
 * and use the compact SHA-256 implementation; algorithms are unchanged.
 */
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_SHA256_SMALLER

#endif /* LM3S6965_MBEDTLS_BOOT_H */
