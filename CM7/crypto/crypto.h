#pragma once

#include <stddef.h>
#include <stdint.h>

/* The hub's asymmetric and key-derivation crypto. Runs on CM7 because a
 * software P-256 scalar multiplication is far too slow for a TDMA slot; CM4
 * keeps only the per-frame symmetric work. See
 * docs/decisions/0011-mbedtls-on-cm7-only.md. */

typedef enum {
    CRYPTO_TEST_DRBG = 0,
    CRYPTO_TEST_HKDF,
    CRYPTO_TEST_GCM,
    CRYPTO_TEST_ECDH,
    CRYPTO_TEST_GCMLEN,
    CRYPTO_TEST_VECTORS,
    CRYPTO_TEST_COMPRESS,
    CRYPTO_TEST_PAIRCOST,
    CRYPTO_TEST_PAIRV2,
    CRYPTO_TEST_PAIRV3,
    CRYPTO_TEST_COUNT
} crypto_test_t;

/* Seeds the DRBG from the hardware RNG. Returns 0 on success. */
int crypto_init(void);

/* The project's randomness entry point. Everything that needs unpredictable
 * bytes goes through here rather than touching the RNG or mbedTLS directly. */
int crypto_random(void *buf, size_t len);

/* Generates a P-256 keypair. `pub` is the compressed SEC1 point, 33 bytes.
 *
 * Both outputs are zeroed on failure. A caller that misses the return value
 * then gets an obviously broken key rather than a plausible one - the same
 * reason rng_bytes zeroes its destination, and the reason the DRBG refuses to
 * seed rather than seeding with something credible. */
int crypto_p256_keygen(uint8_t priv[32], uint8_t pub[33]);

/* Recovers the compressed public key from a stored private one, so the store
 * keeps 32 bytes rather than 65 and nothing has to overload a record field.
 * Costs one scalar multiplication, ~167 ms, on a path that runs at most once
 * per boot. Zeroes `pub` on failure. */
int crypto_p256_public(const uint8_t priv[32], uint8_t pub[33]);

/* Everything one pairing produces on this side. The hop key is not here: it is
 * a network key the hub already holds, not something the exchange derives - see
 * radio_pair_grant_t. */
typedef struct crypto_pair_out {
    uint8_t eph_pub[33];      /* goes out in PAIR_RSP */
    uint8_t key_session[16];  /* seals PAIR_ACCEPT and every uplink report */
    uint8_t confirm_hub[16];  /* goes out in PAIR_RSP */
    uint8_t confirm_dev[16];  /* what PAIR_CONF must contain */
} crypto_pair_out_t;

/* One pairing's worth of arithmetic: two scalar multiplications, four HKDF
 * expansions and two HMACs, against pair_v2.
 *
 * `dev_pub` is checked to be on the curve before anything is derived from it.
 * mbedTLS's point reader succeeds on an x with no square root and hands back a
 * garbage Y - it says so in its own comment - so the curve check is the only
 * thing standing between a hostile 33 bytes and a scalar multiplication.
 *
 * Costs ~330 ms. Zeroes `out` on any failure: a caller that misses the return
 * value then transmits an obviously dead key rather than a plausible one.
 *
 * Returns 0, or a negative mbedTLS error. */
int crypto_pair_derive(const uint8_t hub_priv[32], const uint8_t hub_pub[33],
                       const uint8_t dev_pub[33],
                       uint32_t hub_id, uint32_t dev_id,
                       uint32_t req_superframe, const uint8_t dev_nonce[8],
                       crypto_pair_out_t *out);

const char *crypto_test_name(crypto_test_t t);

/* 0 on pass, otherwise a negative mbedTLS error or -1 for a mismatch. */
/* pair_v3's invitation. crypto_pair_init_key costs a scalar multiplication and
 * belongs once per device; crypto_pair_init_mac runs once per retry. Pinned in
 * Common/test/vectors/pair_v3.h. */
int crypto_pair_init_key(const uint8_t hub_priv[32], const uint8_t dev_pub[33],
                         uint32_t hub_id, uint32_t dev_id, uint8_t k_init[32]);
int crypto_pair_init_mac(const uint8_t k_init[32], const uint8_t *hdr,
                         size_t hdr_len, uint8_t mac[12]);

int crypto_run_test(crypto_test_t t);
