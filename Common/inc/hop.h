#pragma once

#include <stdint.h>

/**
 * @file hop.h
 * @brief Channel selection from the counter and a shared secret, indexed not stepped.
 *
 * radio_devices_docs/radio/hopping.md
 */

#define HOP_MAX_CHANNELS 64

/**
 * @brief One AES-128 block over the 16 bytes as given, and it must be able to fail.
 * @param ctx  the implementation's own context
 * @param in   the block to encrypt
 * @param out  receives the ciphertext block
 * @retval 0   the block was produced
 * @retval !=0 the caller must not transmit
 *
 * radio_devices_docs/radio/hopping.md
 */
typedef int (*hop_prf_t)(void *ctx, const uint8_t in[16], uint8_t out[16]);

typedef struct {
    hop_prf_t prf;
    void     *prf_ctx;
    uint8_t   count;                        /**< channels in the plan */
    uint8_t   deck[HOP_MAX_CHANNELS];       /**< permutation for the cached cycle */
    uint32_t  cycle;                        /**< which cycle deck holds */
    uint8_t   valid;
} hop_ctx_t;

/**
 * @brief Binds a PRF and a channel count to a hopping context.
 * @param ctx      the context to initialise
 * @param prf      the block function the channel derives through
 * @param prf_ctx  passed back to @p prf untouched
 * @param count    channels in the plan, 2..HOP_MAX_CHANNELS
 * @retval  0  the context is usable
 * @retval -1  a null argument or a count outside the range
 */
int hop_init(hop_ctx_t *ctx, hop_prf_t prf, void *prf_ctx, uint8_t count);

/**
 * @brief The channel a superframe falls on, deriving the cycle's deck if needed.
 * @param ctx         an initialised context
 * @param superframe  the counter the sequence is indexed by, not stepped by
 * @param channel     receives the grid slot
 * @retval  0  @p channel holds the answer
 * @retval -1  the PRF failed; the caller must not transmit
 *
 * radio_devices_docs/radio/hopping.md
 */
int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel);
