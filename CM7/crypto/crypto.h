#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @file crypto.h
 * @brief Asymmetric and key-derivation crypto, on CM7 because P-256 is. ADR-0011
 */

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
    CRYPTO_TEST_PAIRPROV,
    CRYPTO_TEST_COUNT
} crypto_test_t;

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
 * @brief Generates a P-256 keypair.
 * @param priv  receives the scalar, zeroed on failure
 * @param pub   receives the compressed point, also zeroed on failure
 * @retval  0  generated
 * @retval !=0 an mbedTLS error, and neither output is half-written
 */
int crypto_p256_keygen(uint8_t priv[32], uint8_t pub[33]);

/**
 * @brief Recovers the compressed public key from a stored private one, ~167 ms.
 * @param priv  the stored scalar
 * @param pub   receives the compressed point
 * @retval  0  recovered
 * @retval !=0 an mbedTLS error
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
int crypto_p256_public(const uint8_t priv[32], uint8_t pub[33]);

/**
 * @brief Everything one pairing produces here; the hop key is not among them.
 *
 * radio_devices_docs/radio/crypto/key-lifecycle.md
 */
typedef struct crypto_pair_out {
    uint8_t eph_pub[33];      /**< goes out in PAIR_RSP */
    uint8_t key_session[16];  /**< seals PAIR_ACCEPT and every uplink report */
    uint8_t confirm_hub[16];  /**< goes out in PAIR_RSP */
    uint8_t confirm_dev[16];  /**< what PAIR_CONF must contain */
    uint8_t transcript[119];    /**< the bytes the confirmations were taken over */
} crypto_pair_out_t;

/**
 * @brief One pairing's arithmetic, ~330 ms, with dev_pub curve-checked first.
 * @param hub_priv        this hub's scalar
 * @param hub_pub         its compressed point, bound into the transcript
 * @param dev_pub         the device's, refused unless it is on the curve
 * @param hub_id          bound into the salt, and byte order differs from the wire
 * @param dev_id          the enrolled device
 * @param req_superframe  the request's own field, not the live counter
 * @param dev_nonce       the device's contribution to freshness
 * @param out             receives the session key, both confirmations and the transcript
 * @retval  0  every output is filled
 * @retval !=0 an mbedTLS error, or a point that is not on the curve
 *
 * radio_devices_docs/open_hub/security/self-tests.md
 */
int crypto_pair_derive(const uint8_t hub_priv[32], const uint8_t hub_pub[33],
                       const uint8_t dev_pub[33],
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
 * @brief Derives pair_v3's invitation key, once per device rather than per retry.
 * @param hub_priv  this hub's scalar
 * @param dev_pub   the enrolled device's compressed point
 * @param hub_id    bound into the derivation
 * @param dev_id    the device the invitation is for
 * @param k_init    receives the key
 * @retval  0  derived
 * @retval !=0 an mbedTLS error, or a point not on the curve
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
int crypto_pair_init_key(const uint8_t hub_priv[32], const uint8_t dev_pub[33],
                         uint32_t hub_id, uint32_t dev_id, uint8_t k_init[32]);

/**
 * @brief MACs one invitation header under the key, run once per retry.
 * @param k_init   from crypto_pair_init_key()
 * @param hdr      the header bytes as they go on the wire
 * @param hdr_len  their length
 * @param mac      receives the truncated HMAC-SHA256
 * @retval  0  produced
 * @retval !=0 an mbedTLS error
 */
int crypto_pair_init_mac(const uint8_t k_init[32], const uint8_t *hdr,
                         size_t hdr_len, uint8_t mac[12]);

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
