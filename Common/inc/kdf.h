#pragma once

#include <stdint.h>

/**
 * @file kdf.h
 * @brief The two hash operations the key schedule needs, and no more. ADR-0029
 *
 * Each firmware supplies both: the hub from mbedTLS, the device from its own
 * sha256.c. Neither side writes new cryptography.
 *
 * radio_devices_docs/radio/decisions/0029-the-library-declares-four-backends-and-absorbs-no-control.md
 */

/* The hash's width: a MAC that changes width moves every buffer under it. */
#define CRYPTO_SHA256_LEN  32u

/**
 * @brief HKDF-SHA-256, extract and expand in one call, RFC 5869.
 * @param salt      the extract salt; may be NULL only when salt_len is 0
 * @param salt_len  its length
 * @param ikm       the input keying material
 * @param ikm_len   its length
 * @param info      the expand context string, without a terminator
 * @param info_len  its length
 * @param out       receives out_len bytes
 * @param out_len   how many; the caller's, never this file's
 * @retval  0  derived
 * @retval !=0 the backend's own code, and out is not to be used
 */
int crypto_hkdf_sha256(const uint8_t *salt, uint32_t salt_len,
                       const uint8_t *ikm, uint32_t ikm_len,
                       const uint8_t *info, uint32_t info_len,
                       uint8_t *out, uint32_t out_len);

/**
 * @brief HMAC-SHA-256, RFC 2104.
 * @param key      the MAC key
 * @param key_len  its length
 * @param msg      what is authenticated
 * @param msg_len  its length
 * @param out      receives the whole 32-byte tag; a caller truncates, not this
 * @retval  0  computed
 * @retval !=0 the backend's own code, and out is not to be used
 *
 * The array width is in the signature so a too-small caller is a diagnostic
 * rather than a corruption. radio_devices_docs/radio/crypto/wire-crypto.md
 */
int crypto_hmac_sha256(const uint8_t *key, uint32_t key_len,
                       const uint8_t *msg, uint32_t msg_len,
                       uint8_t out[CRYPTO_SHA256_LEN]);
