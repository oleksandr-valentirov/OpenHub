#pragma once

#include <stddef.h>
#include <stdint.h>

/* The salt, the transcript and the point width, from the file that owns them. */
#include "exchange.h"

/**
 * @file crypto.h
 * @brief Asymmetric crypto and the schedule's backend, on CM7. ADR-0011, ADR-0031
 *
 * The schedule itself is not here and must not come back: it is exchange.c,
 * which both firmwares compile.
 * radio_devices_docs/radio/decisions/0031-the-link-layer-is-a-rule-with-two-roles-and-the-session-layer-stays-on-cm7.md
 */

typedef enum {
    CRYPTO_TEST_DRBG = 0,
    CRYPTO_TEST_HKDF,
    CRYPTO_TEST_GCM,
    CRYPTO_TEST_ECDH,
    CRYPTO_TEST_GCMLEN,
    CRYPTO_TEST_VECTORS,
    CRYPTO_TEST_LOWORDER,
    CRYPTO_TEST_PAIRCOST,
    CRYPTO_TEST_PAIRV4,
    CRYPTO_TEST_X25519,
    CRYPTO_TEST_PAIRPROV,
    CRYPTO_TEST_COUNT
} crypto_test_t;

/** How many of the above run at boot; the rest are the curve's and cost seconds. */
#define CRYPTO_TEST_FAST_COUNT  4u

/**
 * @brief Seeds the DRBG from the hardware RNG.
 * @retval  0  seeded
 * @retval !=0 an mbedTLS error; nothing below may be called
 */
int crypto_init(void);

/**
 * @brief The project's randomness entry point.
 * @param buf  receives the bytes
 * @param len  how many
 * @retval  0  filled
 * @retval !=0 an mbedTLS error
 *
 * Nothing else on CM7 touches the RNG or mbedTLS directly, so reseeding and the
 * seed-error checks have one place to live.
 */
int crypto_random(void *buf, size_t len);

/**
 * @brief Generates an X25519 keypair; the point is a u-coordinate, RFC 7748.
 * @param priv  receives the scalar, zeroed on failure
 * @param pub   receives the u-coordinate, also zeroed on failure
 * @retval  0  generated
 * @retval !=0 an mbedTLS error, and neither output is half-written
 */
int crypto_x25519_keygen(uint8_t priv[32], uint8_t pub[32]);

/**
 * @brief Recovers the public u-coordinate from a stored private one, 60 ms measured.
 * @param priv  the stored scalar
 * @param pub   receives the u-coordinate
 * @retval  0  recovered
 * @retval !=0 an mbedTLS error
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
int crypto_x25519_public(const uint8_t priv[32], uint8_t pub[32]);

/**
 * @brief One X25519 shared secret, RFC 7748, little-endian in and out.
 * @param priv      this side's clamped scalar
 * @param peer_pub  the other side's u-coordinate
 * @param shared    receives the 32-byte secret
 * @retval  0  derived
 * @retval !=0 an mbedTLS error, or an all-zero result from a low-order point
 *
 * radio_devices_docs/radio/crypto/wire-crypto.md
 */
int crypto_x25519_ecdh(const uint8_t priv[32], const uint8_t peer_pub[32],
                       uint8_t shared[32]);

/**
 * @brief Everything one pairing produces here; the hop key is not among them.
 *
 * radio_devices_docs/radio/crypto/key-lifecycle.md
 */
typedef struct crypto_pair_out {
    uint8_t eph_pub[32];                   /**< goes out in PAIR_RSP */
    uint8_t key_session[EXCHANGE_KEY_LEN]; /**< seals PAIR_ACCEPT and every uplink */
    uint8_t confirm_hub[EXCHANGE_CONFIRM_LEN]; /**< goes out in PAIR_RSP */
    uint8_t confirm_dev[EXCHANGE_CONFIRM_LEN]; /**< what PAIR_CONF must contain */
    uint8_t transcript[EXCHANGE_TRANSCRIPT_LEN]; /**< what the confirmations covered */
} crypto_pair_out_t;

/**
 * @brief One pairing's curve arithmetic, then the library's schedule over it.
 * @param hub_priv        this hub's scalar
 * @param hub_pub         its u-coordinate, bound into the transcript
 * @param dev_pub         the device's; any 32 bytes is a u-coordinate, RFC 7748
 * @param eph_priv        the ephemeral this exchange draws, supplied by the caller
 * @param eph_pub         its u-coordinate, bound into the transcript and sent
 * @param hub_id          bound into the salt, and byte order differs from the wire
 * @param dev_id          the enrolled device
 * @param req_superframe  the request's own field, not the live counter
 * @param dev_nonce       the device's contribution to freshness
 * @param out             receives the session key, both confirmations and the transcript
 * @retval  0  every output is filled
 * @retval !=0 an mbedTLS error, the all-zero secret a low-order point gives, or
 *             the schedule's own refusal; out is zeroed
 *
 * The ephemeral is a parameter so the core that owns the identity is the core
 * that draws it, and so a self-test can pin only that one input. ADR-0031
 * radio_devices_docs/open_hub/security/self-tests.md
 */
int crypto_pair_keys(const uint8_t hub_priv[32], const uint8_t hub_pub[32],
                     const uint8_t dev_pub[32], const uint8_t eph_priv[32],
                     const uint8_t eph_pub[32],
                     uint32_t hub_id, uint32_t dev_id,
                     uint32_t req_superframe, const uint8_t dev_nonce[8],
                     crypto_pair_out_t *out);

/**
 * @brief The printable name of a self-test, so a failure names itself.
 * @param t  the test
 * @return its name, or a placeholder for a value outside the enum
 */
const char *crypto_test_name(crypto_test_t t);


/**
 * @brief Runs one self-test.
 * @param t  which
 * @retval  0  passed
 * @retval -1  a value mismatched a published vector
 * @return otherwise a negative mbedTLS error
 *
 * This comment used to float eight lines above the function it describes, with
 * two unrelated declarations between. radio_devices_docs/open_hub/security/self-tests.md
 */
int crypto_run_test(crypto_test_t t);

/* Distinguishes "not run yet" from "ran and passed": zero would be a pass.
 * radio_devices_docs/open_hub/security/self-tests.md */
#define CRYPTO_SELFTEST_UNRUN  (-1000)

/**
 * @brief Runs the self-tests cheap enough to sit inside an existing delay.
 * @retval  0  all four passed
 * @retval !=0 the first failure's code; the test is named by crypto_selftest_fast_rc
 *
 * The slow tests are not here: they cost seconds together and would double the
 * window in which a paired device misses beacons after a hub reset.
 * radio_devices_docs/open_hub/security/self-tests.md
 */
int crypto_selftest_fast(void);

/**
 * @brief The last fast self-test verdict, for a reader that did not run it.
 * @param which  receives the failing test when the verdict is non-zero
 * @return CRYPTO_SELFTEST_UNRUN, 0, or the failure code
 */
int crypto_selftest_fast_rc(crypto_test_t *which);
