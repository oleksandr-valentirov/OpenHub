/**
 * @file test_hop.c
 * @brief The hop sequence against published vectors, on a host where it can fail.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hop.h"
#include "hop_v1.h"

static int fails = 0;
static uint8_t ch(hop_ctx_t *c, uint32_t sf) {
    uint8_t v = 0xFF;
    if (hop_channel(c, sf, &v) != 0) { printf("  FAIL: hop_channel reported an error\n"); exit(1); }
    return v;
}

#define CHECK(c, ...) do { if(!(c)){ printf("FAIL %s:%d  ",__FILE__,__LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while(0)

/* A stand-in for the AES block: the property under test is the shuffle. */
static int test_prf(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)(uintptr_t)ctx;
    for (int i = 0; i < 16; i++) { h ^= in[i]; h *= 0x100000001b3ULL; }
    for (int i = 0; i < 16; i++) { h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; out[i] = (uint8_t)(h >> (i % 8 * 8)); }
    return 0;
}

#define N 29

/* Replays the pinned AES blocks, so this pins the shuffle and not the PRF.
 * radio_devices_docs/open_hub/testing/host-tests.md */
static int replay_prf(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    const uint8_t *s0 = HV_STREAM0, *s1 = HV_STREAM1;
    uint32_t cycle = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                     ((uint32_t)in[2] << 8) | (uint32_t)in[3];
    const uint8_t *src;

    (void)ctx;
    if (cycle == 0u)      src = s0;
    else if (cycle == 1u) src = s1;
    else return -1;                       /* only the pinned cycles are replayed */
    memcpy(out, src + (in[15] ? 16 : 0), 16);
    return 0;
}

/* The deck the shared vectors pin, byte for byte, which no property test sees.
 * radio_devices_docs/open_hub/testing/host-tests.md */
static void test_pinned_deck(void) {
    hop_ctx_t c;

    CHECK(hop_init(&c, replay_prf, NULL, HOP_VEC_COUNT) == 0, "init");
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++)
        CHECK(ch(&c, i) == HV_DECK0[i], "cycle 0 slot %u: %u != %u",
              i, ch(&c, i), HV_DECK0[i]);
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++)
        CHECK(ch(&c, HOP_VEC_COUNT + i) == HV_DECK1[i], "cycle 1 slot %u", i);

    /* Cycle 0 is identical under either endian convention; cycle 1 is not.
     * radio_devices_docs/radio/hopping.md */
    CHECK(memcmp(HV_STREAM0, HV_STREAM1, 32) != 0,
          "the two cycles must not produce the same stream");
}

static void test_permutation_per_cycle(void) {
    hop_ctx_t c;
    CHECK(hop_init(&c, test_prf, (void *)1, N) == 0, "init");
    for (uint32_t cycle = 0; cycle < 50; cycle++) {
        int seen[N] = {0};
        for (uint32_t i = 0; i < N; i++) {
            uint8_t v = ch(&c, cycle * N + i);
            CHECK(v < N, "channel %u out of range", v);
            seen[v]++;
        }
        for (int i = 0; i < N; i++)
            CHECK(seen[i] == 1, "cycle %u: channel %d used %d times", cycle, i, seen[i]);
    }
}

static void test_deterministic_and_stateless(void) {
    hop_ctx_t a, b;
    hop_init(&a, test_prf, (void *)7, N);
    hop_init(&b, test_prf, (void *)7, N);
    /* b jumps straight to a far superframe, as a node waking from sleep would. */
    for (uint32_t i = 0; i < 400; i++) (void)ch(&a, i);
    for (uint32_t i = 380; i < 400; i++)
        CHECK(ch(&a, i) == ch(&b, i), "sf %u differs after a jump", i);
    CHECK(ch(&b, 100000) == ch(&a, 100000), "far jump differs");
}

static void test_key_changes_sequence(void) {
    hop_ctx_t a, b;
    int same = 0;
    hop_init(&a, test_prf, (void *)1, N);
    hop_init(&b, test_prf, (void *)2, N);
    for (uint32_t i = 0; i < 200; i++)
        if (ch(&a, i) == ch(&b, i)) same++;
    CHECK(same < 40, "two seeds gave %d/200 identical channels", same);
}

static void test_not_linear(void) {
    hop_ctx_t c;
    int adjacent = 0, repeats = 0, extrapolated = 0;
    uint8_t seq[600];
    hop_init(&c, test_prf, (void *)3, N);
    for (uint32_t i = 0; i < 600; i++) seq[i] = ch(&c, i);
    for (int i = 0; i + 1 < 600; i++) {
        int d = seq[i+1] - seq[i]; if (d < 0) d = -d;
        if (d == 0) repeats++;
        if (d <= 2) adjacent++;
    }
    for (int i = 0; i + 2 < 600; i++)
        if (seq[i+2] == (uint8_t)((2 * seq[i+1] + N - seq[i]) % N)) extrapolated++;
    /* Not zero: repeats are guaranteed absent within a cycle, not across one.
     * radio_devices_docs/open_hub/testing/host-tests.md */
    CHECK(repeats * 100 < 600, "%d/599 hops stayed on the same channel", repeats);
    CHECK(adjacent < 600 / 5, "%d/599 hops landed within 200 kHz", adjacent);
    CHECK(extrapolated < 600 / 8, "%d/598 hops guessable by extrapolation", extrapolated);
    printf("  spread: %d repeats, %d adjacent, %d extrapolated (of ~600)\n",
           repeats, adjacent, extrapolated);
}

static void test_bad_args(void) {
    hop_ctx_t c;
    CHECK(hop_init(&c, test_prf, NULL, 1) != 0, "count 1 must be rejected");
    CHECK(hop_init(&c, test_prf, NULL, HOP_MAX_CHANNELS + 1) != 0, "count 65 must be rejected");
    CHECK(hop_init(&c, NULL, NULL, N) != 0, "missing prf must be rejected");
    CHECK(hop_init(NULL, test_prf, NULL, N) != 0, "null ctx must be rejected");
}

/* Asserts the cost, not the answer: two blocks per cycle.
 * radio_devices_docs/open_hub/testing/host-tests.md */
static unsigned prf_calls;

static int counting_prf(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    prf_calls++;
    return replay_prf(ctx, in, out);
}

static void test_prf_call_cost(void) {
    hop_ctx_t c;

    CHECK(hop_init(&c, counting_prf, NULL, HOP_VEC_COUNT) == 0, "init");

    prf_calls = 0;
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++)
        (void)ch(&c, i);
    CHECK(prf_calls == 2, "cycle 0 took %u PRF calls, not 2", prf_calls);

    /* The whole of the next cycle costs one more deck and no more. */
    prf_calls = 0;
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++)
        (void)ch(&c, HOP_VEC_COUNT + i);
    CHECK(prf_calls == 2, "cycle 1 took %u PRF calls, not 2", prf_calls);

    /* Re-reading inside the cached cycle must cost nothing. */
    prf_calls = 0;
    for (int k = 0; k < 5; k++)
        (void)ch(&c, HOP_VEC_COUNT + 3u);
    CHECK(prf_calls == 0, "a repeat lookup inside the cached cycle cost %u calls",
          prf_calls);

    /* An evicted cycle must rebuild: the cache is one deck. */
    prf_calls = 0;
    (void)ch(&c, 0);
    CHECK(prf_calls == 2, "returning to cycle 0 took %u calls, not 2", prf_calls);
}

static void test_pinned_samples(void) {
    hop_ctx_t c;

    /* Far-apart superframes, the counter's last value included.
     * radio_devices_docs/radio/hopping.md */
    CHECK(hop_init(&c, replay_prf, NULL, HOP_VEC_COUNT) == 0, "init");
    for (unsigned i = 0; i < sizeof(HV_SAMPLE_CH); i++) {
        uint32_t sf = HV_SAMPLE_SF[i];
        uint8_t v = 0xFF;

        if (sf / HOP_VEC_COUNT > 1u)
            continue;                      /* outside the replayed cycles */
        CHECK(hop_channel(&c, sf, &v) == 0, "sf %u", sf);
        CHECK(v == HV_SAMPLE_CH[i], "sf %u: %u != %u", sf, v, HV_SAMPLE_CH[i]);
    }
}

int main(void) {
    test_pinned_deck();
    test_pinned_samples();
    test_prf_call_cost();
    test_permutation_per_cycle();
    test_deterministic_and_stateless();
    test_key_changes_sequence();
    test_not_linear();
    test_bad_args();
    if (fails == 0) printf("all hop tests passed\n");
    else printf("%d check(s) failed\n", fails);
    return fails != 0;
}
