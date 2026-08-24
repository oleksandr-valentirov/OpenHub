#pragma once

#include <stdint.h>

#include "kdf.h"
#include "radio_protocol.h"

/* The pair_v4 key schedule: the spec's text, no HAL, no hash of its own.
 * radio_devices_docs/wl55_device/security/self-tests.md */

#define EXCHANGE_Z_TERM_LEN      32u
#define EXCHANGE_Z_LEN           (2u * EXCHANGE_Z_TERM_LEN)
#define EXCHANGE_NONCE_LEN       8u
#define EXCHANGE_CONFIRM_LEN     16u
#define EXCHANGE_CONFIRM_KEY_LEN CRYPTO_SHA256_LEN
#define EXCHANGE_KEY_LEN         16u

/* From the wire struct, never 33: a point that changes width moves the transcript.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#define EXCHANGE_POINT_LEN \
    ((uint32_t)sizeof(((radio_pair_req_t *)0)->pubkey))

/* hub_id | dev_id | superframe | dev_nonce, big-endian throughout. */
#define EXCHANGE_SALT_LEN        (4u + 4u + 4u + EXCHANGE_NONCE_LEN)

/* Hub keys before the device's: whichever key is not bound is not bound.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#define EXCHANGE_TRANSCRIPT_LEN  (EXCHANGE_SALT_LEN + 3u * EXCHANGE_POINT_LEN)

/* No hop key here: a network key derived here would be a key with no consumer.
 * radio_devices_docs/wl55_device/security/self-tests.md */
typedef struct {
    uint8_t session[EXCHANGE_KEY_LEN];
    uint8_t confirm_key_hub[EXCHANGE_CONFIRM_KEY_LEN];
    uint8_t confirm_key_dev[EXCHANGE_CONFIRM_KEY_LEN];
    uint8_t confirm_hub[EXCHANGE_CONFIRM_LEN];
    uint8_t confirm_dev[EXCHANGE_CONFIRM_LEN];
} exchange_keys_t;

/** @brief Builds the HKDF salt, separately from the transcript on purpose.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
void exchange_salt(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                   const uint8_t *dev_nonce, uint8_t *out);

/** @brief Hashes the transcript both sides must agree on. */
void exchange_transcript(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                         const uint8_t *dev_nonce,
                         const uint8_t *hub_static_c,
                         const uint8_t *hub_eph_c,
                         const uint8_t *dev_static_c,
                         uint8_t *out);

/**
 * @brief Derives the session and confirmation keys; Z arrives as two terms.
 * @param z1          the static-static term
 * @param z2          the ephemeral term
 * @param salt        EXCHANGE_SALT_LEN bytes from exchange_salt
 * @param transcript  EXCHANGE_TRANSCRIPT_LEN bytes from exchange_transcript
 * @param out         receives every key and both confirmations
 * @retval  0  every field is filled
 * @retval !=0 the backend's code; out is zeroed and nothing in it may be used
 *
 * The backend can fail on one side and not the other, so the return is checked
 * rather than assumed. radio_devices_docs/radio/crypto/key-lifecycle.md
 */
int exchange_derive(const uint8_t *z1, const uint8_t *z2,
                    const uint8_t *salt, const uint8_t *transcript,
                    exchange_keys_t *out);

/** @brief Constant-time compare; returns 1 on a match, and acts on nothing. */
int exchange_confirm_equal(const uint8_t *a, const uint8_t *b);

/** @brief The hub reusing its static key as its ephemeral; NOT a freshness check.
 *  radio_devices_docs/wl55_device/radio/pairing.md */
int exchange_eph_is_static(const uint8_t *hub_eph_c, const uint8_t *hub_static_c);
