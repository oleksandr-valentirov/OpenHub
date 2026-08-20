#include <string.h>

#include "hop.h"

int hop_init(hop_ctx_t *ctx, hop_prf_t prf, void *prf_ctx, uint8_t count) {
    if (ctx == NULL || prf == NULL || count < 2 || count > HOP_MAX_CHANNELS)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->prf = prf;
    ctx->prf_ctx = prf_ctx;
    ctx->count = count;
    return 0;
}

/* Fisher-Yates over the channel list, seeded by the PRF of the cycle number.
 * Two blocks give 32 bytes, enough randomness for up to 64 channels. */
static int build_deck(hop_ctx_t *ctx, uint32_t cycle) {
    uint8_t block[16];
    uint8_t stream[32];
    uint8_t i;

    /* Dropped before rebuilding, so a deck half-built by a failing PRF can never
     * be served as though it were cached. */
    ctx->valid = 0;

    memset(block, 0, sizeof(block));
    memset(stream, 0, sizeof(stream));

    /* Big-endian: this is a crypto input, and the rule is that anything fed into
     * the crypto layer is big-endian regardless of how it sits on the wire.
     * Cycle 0 is identical either way, which is exactly why a first-contact test
     * inside the first cycle would pass and everything after it would not. */
    block[0] = (uint8_t)(cycle >> 24);
    block[1] = (uint8_t)(cycle >> 16);
    block[2] = (uint8_t)(cycle >> 8);
    block[3] = (uint8_t)cycle;
    if (ctx->prf(ctx->prf_ctx, block, stream) != 0)
        return -1;
    block[15] = 1;
    if (ctx->prf(ctx->prf_ctx, block, stream + 16) != 0)
        return -1;

    for (i = 0; i < ctx->count; i++)
        ctx->deck[i] = i;

    for (i = (uint8_t)(ctx->count - 1); i > 0; i--) {
        /* Rejection-free enough here: the modulo bias over a 0..255 draw for
         * i+1 <= 64 is under 0.4%, and the deck stays a permutation regardless. */
        uint8_t j = (uint8_t)(stream[i & 31] % (i + 1));
        uint8_t t = ctx->deck[i];
        ctx->deck[i] = ctx->deck[j];
        ctx->deck[j] = t;
    }
    ctx->cycle = cycle;
    ctx->valid = 1;
    return 0;
}

int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel) {
    uint32_t cycle;

    if (ctx == NULL || channel == NULL)
        return -1;

    cycle = superframe / ctx->count;
    if ((!ctx->valid || ctx->cycle != cycle) && build_deck(ctx, cycle) != 0)
        return -1;

    *channel = ctx->deck[superframe % ctx->count];
    return 0;
}
