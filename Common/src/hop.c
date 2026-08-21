/**
 * @file hop.c
 * @brief The hop sequence: a deck per cycle, indexed by the counter rather than stepped.
 *
 * radio_devices_docs/radio/hopping.md
 */
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

/* Fisher-Yates seeded by the PRF of the cycle number; two blocks, 32 bytes. */
static int build_deck(hop_ctx_t *ctx, uint32_t cycle) {
    uint8_t block[16];
    uint8_t stream[32];
    uint8_t i;

    /* Dropped before rebuilding: a half-built deck must not be served. */
    ctx->valid = 0;

    memset(block, 0, sizeof(block));
    memset(stream, 0, sizeof(stream));

    /* Big-endian, as every crypto input is; cycle 0 is identical either way.
     * radio_devices_docs/radio/hopping.md */
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
        /* Modulo bias under 0.4% here, and the deck stays a permutation. */
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
