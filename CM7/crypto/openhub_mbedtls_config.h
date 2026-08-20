#pragma once

/* mbedTLS build for the hub's CM7 core.
 *
 * Scope for now is the radio's crypto: P-256 ECDH at pairing, HKDF-SHA256 for
 * key derivation and the daily ratchet, AES-128-GCM for frames. TLS for Ethernet
 * lands later and only adds to this file - see docs/network/tls.md.
 *
 * Asymmetric work runs here rather than on CM4 because a software P-256 scalar
 * multiplication is tens of milliseconds and would blow a TDMA slot. */

/* --- platform ---------------------------------------------------------- */
#include <stddef.h>

/* Pinned to the FreeRTOS heap. newlib_stubs.c overrides malloc/free but not
 * calloc, so the default pairing would allocate from one heap and free into
 * another - see CM7/crypto/mbedtls_alloc.c. */
void *openhub_calloc(size_t n, size_t size);
void  openhub_free(void *p);
#define MBEDTLS_PLATFORM_CALLOC_MACRO  openhub_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO    openhub_free

/* No filesystem, no sockets, no time source, no /dev/urandom. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

/* Entropy comes from the guarded RNG service, never from the HAL directly:
 * on this part the HAL cannot report a seed error at all. See CM7/crypto/entropy_hw.c
 * and docs/security/entropy.md. */
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
/* P-256 only. The device side accelerates it in PKA hardware and has no
 * accelerator for anything else - see docs/decisions/0010-p256-over-x25519.md. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* --- diagnostics ------------------------------------------------------- */
/* Self-tests are the only on-target check available while there is no device to
 * talk to; the `crypto` console command runs them. */
#define MBEDTLS_SELF_TEST
#define MBEDTLS_ERROR_C
