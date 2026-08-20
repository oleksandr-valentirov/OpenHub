#pragma once

#include <stdint.h>

/* Channel selection for the hopping sequence.
 *
 * Both ends must land on the same channel, so the sequence is a deterministic
 * function of the superframe counter and a secret shared at pairing. The RNG
 * belongs at pairing time, producing that secret - never per hop, because a
 * value only the hub knows is a value the device cannot follow.
 *
 * Indexing by the counter rather than stepping an LFSR keeps it stateless: a
 * node that slept through a thousand superframes reads the counter from the
 * beacon and computes the current channel directly, with nothing to fast-forward. */

#define HOP_MAX_CHANNELS 64

/* One AES-128 block over the 16 bytes as given - not word-swapped. Returns 0 on
 * success. It must be able to fail: a hardware accelerator shared with the frame
 * cipher can be busy or misconfigured, and Fisher-Yates over an uninitialised
 * buffer still produces a perfectly valid permutation, so a silent failure looks
 * exactly like a working hop sequence that no device can follow. */
typedef int (*hop_prf_t)(void *ctx, const uint8_t in[16], uint8_t out[16]);

typedef struct {
    hop_prf_t prf;
    void     *prf_ctx;
    uint8_t   count;                        /* channels in the plan */
    uint8_t   deck[HOP_MAX_CHANNELS];       /* permutation for the cached cycle */
    uint32_t  cycle;                        /* which cycle deck holds */
    uint8_t   valid;
} hop_ctx_t;

/* count must be 2..HOP_MAX_CHANNELS. Returns 0 on success. */
int hop_init(hop_ctx_t *ctx, hop_prf_t prf, void *prf_ctx, uint8_t count);

/* Channel for a superframe, written to *channel. Returns 0 on success, non-zero
 * if the PRF failed - in which case no channel is produced and the caller must
 * not transmit rather than guess.
 *
 * Every channel is used exactly once per `count` superframes, so occupancy stays
 * uniform and the revisit interval is bounded - the two properties a plain
 * PRF-mod-N throws away. Note this holds *within* a cycle: the last channel of
 * one deck equals the first of the next with probability 1/count. */
int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel);
