#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @file aead.h
 * @brief AES-128-GCM through CRYP, stating its whole configuration every time.
 *
 * Inheriting configuration from the other CRYP user is what produced two wrong
 * answers from two correct functions. radio_devices_docs/radio/crypto/wire-crypto.md
 */

#define AEAD_KEY_BYTES    16u
#define AEAD_NONCE_BYTES  12u
#define AEAD_TAG_BYTES    16u

/**
 * @brief Builds the wire nonce: superframe(4)||dev_id(4)||direction(1)||slot(3).
 * @param out         receives the nonce, big-endian throughout
 * @param superframe  the counter the frame is sealed under
 * @param dev_id      the device the slot was granted to
 * @param direction   uplink or downlink, which is what keeps the two apart
 * @param slot        the slot inside the superframe
 */
void aead_nonce(uint8_t out[AEAD_NONCE_BYTES], uint32_t superframe,
                uint32_t dev_id, uint8_t direction, uint32_t slot);

/**
 * @brief Seals a payload, producing ciphertext and its tag.
 * @param key      the session or pairing key
 * @param nonce    from aead_nonce(), never reused under one key
 * @param aad      additional data, authenticated but not encrypted
 * @param aad_len  its length
 * @param pt       the plaintext
 * @param len      its length; @p ct may be NULL when this is 0
 * @param ct       receives the ciphertext, may alias @p pt
 * @param tag      receives the authentication tag
 * @retval  0  sealed
 * @retval !=0 CRYP refused; nothing was transmitted
 */
int aead_seal(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[AEAD_TAG_BYTES]);

/**
 * @brief Opens a frame, verifying the tag before the plaintext is usable.
 * @param key      the key the sender is believed to hold
 * @param nonce    reconstructed from the frame's own header fields
 * @param aad      additional data as the sender authenticated it
 * @param aad_len  its length
 * @param ct       the ciphertext
 * @param len      its length
 * @param pt       receives the plaintext, and is zeroed on any failure
 * @param tag      the tag as received
 * @retval  0  the tag verified
 * @retval !=0 it did not; @p pt is zeroed rather than left half-written
 */
int aead_open(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *ct, size_t len, uint8_t *pt,
              const uint8_t tag[AEAD_TAG_BYTES]);

/**
 * @brief Runs the published pair_v2 frames through seal and open.
 * @retval  0  every vector matched
 * @retval !=0 the index of the first that did not
 *
 * Whole frames rather than a round trip: a round trip agrees with itself under
 * any self-consistent assembly. radio_devices_docs/open_hub/security/self-tests.md
 */
int aead_selftest(void);
