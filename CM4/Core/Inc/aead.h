#pragma once

#include <stdint.h>
#include <stddef.h>

/* AES-128-GCM through the CRYP accelerator, which is the only thing on this
 * board that can seal a frame inside a slot.
 *
 * Nonces are built rather than transmitted: every field is already agreed by
 * both ends, so the nonce costs no air time and cannot be manipulated in
 * flight. See docs/security/wire-crypto.md.
 *
 * CRYP is stateful and has two users here - this and the hop PRF. CM4 has no
 * RTOS, so both run to completion inside one superloop pass and cannot
 * interleave; each sets the whole config before use rather than relying on
 * what the other left behind. That is serialisation by construction and it
 * stops being true the moment anything on this core preempts. */

#define AEAD_KEY_BYTES    16u
#define AEAD_NONCE_BYTES  12u
#define AEAD_TAG_BYTES    16u

/* superframe(4) || dev_id(4) || direction(1) || slot(3), big-endian. */
void aead_nonce(uint8_t out[AEAD_NONCE_BYTES], uint32_t superframe,
                uint32_t dev_id, uint8_t direction, uint32_t slot);

/* 0 on success. `ct` may be NULL when len is 0. */
int aead_seal(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[AEAD_TAG_BYTES]);

/* 0 when the tag verified and `pt` holds the plaintext; non-zero otherwise, and
 * `pt` is zeroed rather than left holding unauthenticated bytes. */
int aead_open(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *ct, size_t len, uint8_t *pt,
              const uint8_t tag[AEAD_TAG_BYTES]);

/* Runs the published pair_v2 frames through the two functions above. 0 on pass.
 * Costs a few hundred microseconds and runs once at boot, because a radio that
 * cannot seal a frame correctly should not transmit one. */
int aead_selftest(void);
