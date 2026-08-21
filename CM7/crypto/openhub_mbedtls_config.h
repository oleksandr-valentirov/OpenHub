#pragma once

/* mbedTLS for CM7: radio crypto now, TLS later. radio_devices_docs/open_hub/security/ */

/* --- platform ---------------------------------------------------------- */
#include <stddef.h>

/* Pinned to the FreeRTOS heap; see CM7/crypto/mbedtls_alloc.c. */
void *openhub_calloc(size_t n, size_t size);
void  openhub_free(void *p);
#define MBEDTLS_PLATFORM_CALLOC_MACRO  openhub_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO    openhub_free

/* No filesystem, no sockets, no time source, no /dev/urandom. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

/* Entropy from the guarded RNG service, not the HAL. radio_devices_docs/open_hub/security/entropy.md */
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* --- random ------------------------------------------------------------ */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* --- hashing ----------------------------------------------------------- */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_HKDF_C

/* --- symmetric --------------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C

/* --- elliptic curve ---------------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
/* P-256 only. ADR-0010 */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* --- diagnostics ------------------------------------------------------- */

/* Self-tests, run by the `crypto` console command.
 * radio_devices_docs/open_hub/security/self-tests.md */
#define MBEDTLS_SELF_TEST
#define MBEDTLS_ERROR_C
