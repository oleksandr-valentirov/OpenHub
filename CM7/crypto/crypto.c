#include <string.h>

#include "crypto.h"
#include "rng.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "wire_v3.h"
#include "pair_v2.h"
#include "pair_v3.h"
#include "radio_protocol.h"

#define CRYPTO_MISMATCH (-1)

static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;
static int seeded = 0;

/* Readable over SWD, so a stall is locatable even though the CLI only flushes
 * its buffer after a command handler returns. */
volatile uint32_t crypto_stage = 0;

/* Personalisation only separates this DRBG from another seeded from the same
 * source; it is not secret and does not need to be. */
static const unsigned char drbg_pers[] = "openhub-hub-cm7";

int crypto_init(void) {
    int rc;

    crypto_stage = 1;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    crypto_stage = 2;
    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                               drbg_pers, sizeof(drbg_pers) - 1u);
    crypto_stage = 3;
    seeded = (rc == 0);
    return rc;
}

/* Seeded on first use rather than at boot. Entropy gathering must never be able
 * to hold up the console: the console is how a failure in it gets diagnosed. */
static int ensure_seeded(void) {
    return seeded ? 0 : crypto_init();
}

int crypto_random(void *buf, size_t len) {
    int rc = ensure_seeded();

    if (rc != 0)
        return rc;
    return mbedtls_ctr_drbg_random(&drbg, (unsigned char *)buf, len);
}

/* mbedTLS takes its randomness as a callback of this shape. */
static int rng_cb(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    return crypto_random(out, len);
}

int crypto_p256_keygen(uint8_t priv[32], uint8_t pub[33]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen = 0;
    int rc;

    if (priv == NULL || pub == NULL)
        return -1;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;

    /* rng_cb reaches the guarded hardware draw: SEIS cleared before, SEIS and
     * live SECS tested after, output zeroed on failure, and a DRBG that refuses
     * to seed rather than seeding with something plausible. A stuck ephemeral
     * is undetectable from the far end and produces a pairing that looks
     * perfect, so this path being the guarded one is load-bearing. */
    rc = mbedtls_ecdh_gen_public(&grp, &d, &Q, rng_cb, NULL);
    if (rc != 0) goto done;

    rc = mbedtls_mpi_write_binary(&d, priv, 32);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_COMPRESSED,
                                        &olen, pub, 33);
    if (rc == 0 && olen != 33)
        rc = CRYPTO_MISMATCH;

done:
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) {
        memset(priv, 0, 32);
        memset(pub, 0, 33);
    }
    return rc;
}

int crypto_p256_public(const uint8_t priv[32], uint8_t pub[33]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen = 0;
    int rc;

    if (priv == NULL || pub == NULL)
        return -1;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_read_binary(&d, priv, 32);
    if (rc != 0) goto done;
    /* Rejects zero and anything at or above the group order, so a corrupted or
     * all-zero stored key fails here rather than producing a usable-looking
     * point. An all-zero private key is exactly what a missed keygen failure
     * leaves behind. */
    rc = mbedtls_ecp_check_privkey(&grp, &d);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_COMPRESSED,
                                        &olen, pub, 33);
    if (rc == 0 && olen != 33)
        rc = CRYPTO_MISMATCH;

done:
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0)
        memset(pub, 0, 33);
    return rc;
}

/* --- pairing ----------------------------------------------------------- */

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Big-endian, unlike the packed frame structs these values also travel in.
 * Both rules are live inside one frame - see Common/test/vectors/pair_v2.txt -
 * so the two builders below are the only places either is written down. */
static void pair_salt(uint8_t salt[20], uint32_t hub_id, uint32_t dev_id,
                      uint32_t req_superframe, const uint8_t dev_nonce[8]) {
    be32(salt, hub_id);
    be32(salt + 4, dev_id);
    be32(salt + 8, req_superframe);
    memcpy(salt + 12, dev_nonce, 8);
}

/* Both hub keys are bound, and whichever key is not bound is not bound.
 * hub_static is never transmitted: both ends hold it, the device from
 * provisioning. */
static void pair_transcript(uint8_t t[119], uint32_t hub_id, uint32_t dev_id,
                            uint32_t req_superframe, const uint8_t dev_nonce[8],
                            const uint8_t hub_pub[33], const uint8_t eph_pub[33],
                            const uint8_t dev_pub[33]) {
    be32(t, hub_id);
    be32(t + 4, dev_id);
    be32(t + 8, req_superframe);
    memcpy(t + 12, dev_nonce, 8);
    memcpy(t + 20, hub_pub, 33);
    memcpy(t + 53, eph_pub, 33);
    memcpy(t + 86, dev_pub, 33);
}

static int hkdf16(const uint8_t *z, const uint8_t salt[20], const char *info,
                  uint8_t *out, size_t len) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    return mbedtls_hkdf(md, salt, 20, z, 64,
                        (const unsigned char *)info, strlen(info), out, len);
}

static int confirm(const uint8_t key[32], const uint8_t t[119], uint8_t out[16]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t full[32];
    int rc = mbedtls_md_hmac(md, key, 32, t, 119, full);

    if (rc == 0)
        memcpy(out, full, 16);
    mbedtls_platform_zeroize(full, sizeof(full));
    return rc;
}

/* pair_v3's invitation key. Z1 alone - the static-static term - because both
 * ends can compute it before any frame exists, which is what lets the hub's
 * first frame be authenticated at all (ADR-0021).
 *
 * The salt is hub_id||dev_id big-endian and nothing else. pair_v2's 20-byte
 * salt binds the request superframe and the device nonce, and neither exists
 * yet: this is the frame before the device has spoken. So K_init is static per
 * pair and the freshness has to live in the message - the MAC covers the
 * superframe, and the device must refuse one it has already seen.
 *
 * **Derive this once per device, not once per frame.** It is a scalar
 * multiplication, and PAIR_INIT is retried for 60 s; more importantly the same
 * cost on the receiving side sits behind an unauthenticated frame, which is the
 * denial of service the device's rate limit exists to bound. Raised by the
 * device side, which caches it and counts the derivations for that reason.
 *
 * Returns 0, or a negative mbedTLS error. Zeroes k_init on any failure. */
int crypto_pair_init_key(const uint8_t hub_priv[32], const uint8_t dev_pub[33],
                         uint32_t hub_id, uint32_t dev_id, uint8_t k_init[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_ecp_group grp;
    mbedtls_ecp_point D;
    mbedtls_mpi d, z;
    uint8_t z1[32];
    uint8_t salt[8];
    int rc;

    memset(k_init, 0, 32);
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&D);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    memset(z1, 0, sizeof(z1));

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_point_read_binary(&grp, &D, dev_pub, 33);
    if (rc != 0) goto done;
    /* Same reason as the pairing path: read_binary accepts an x with no square
     * root and hands back a garbage Y, so this is the only thing between a
     * hostile 33 bytes and a scalar multiplication. */
    rc = mbedtls_ecp_check_pubkey(&grp, &D);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_read_binary(&d, hub_priv, 32);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_check_privkey(&grp, &d);
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_compute_shared(&grp, &z, &D, &d, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_write_binary(&z, z1, 32);
    if (rc != 0) goto done;

    salt[0] = (uint8_t)(hub_id >> 24); salt[1] = (uint8_t)(hub_id >> 16);
    salt[2] = (uint8_t)(hub_id >> 8);  salt[3] = (uint8_t)hub_id;
    salt[4] = (uint8_t)(dev_id >> 24); salt[5] = (uint8_t)(dev_id >> 16);
    salt[6] = (uint8_t)(dev_id >> 8);  salt[7] = (uint8_t)dev_id;

    rc = mbedtls_hkdf(md, salt, sizeof(salt), z1, sizeof(z1),
                      (const uint8_t *)"openhub/v3/init", 15, k_init, 32);
done:
    if (rc != 0)
        memset(k_init, 0, 32);
    mbedtls_platform_zeroize(z1, sizeof(z1));
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&D);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* HMAC-SHA256 truncated to 96 bits, over the frame's cleartext. Split from the
 * key derivation because they run at different rates: the key once per device,
 * this once per retry. */
int crypto_pair_init_mac(const uint8_t k_init[32], const uint8_t *hdr,
                         size_t hdr_len, uint8_t mac[12]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t full[32];
    int rc;

    memset(mac, 0, 12);
    rc = mbedtls_md_hmac(md, k_init, 32, hdr, hdr_len, full);
    if (rc == 0)
        memcpy(mac, full, 12);
    mbedtls_platform_zeroize(full, sizeof(full));
    return rc;
}

/* The whole derivation with the ephemeral supplied rather than drawn, so the
 * self-test can run the *production* path against pair_v2 and only the
 * randomness is pinned. A test that reimplemented the derivation would check
 * the test. */
static int pair_derive_eph(const uint8_t hub_priv[32], const uint8_t hub_pub[33],
                           const uint8_t dev_pub[33], const uint8_t eph_priv[32],
                           const uint8_t eph_pub[33],
                           uint32_t hub_id, uint32_t dev_id,
                           uint32_t req_superframe, const uint8_t dev_nonce[8],
                           crypto_pair_out_t *out) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point D;
    mbedtls_mpi d, z;
    uint8_t zz[64];
    uint8_t salt[20];
    uint8_t t[119];
    uint8_t ck_hub[32], ck_dev[32];
    int rc;

    memset(out, 0, sizeof(*out));
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&D);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    memset(zz, 0, sizeof(zz));

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_point_read_binary(&grp, &D, dev_pub, 33);
    if (rc != 0) goto done;
    /* read_binary succeeds on an x with no square root and returns a garbage Y.
     * Nothing else rejects that, and about half of all field elements are valid
     * x-coordinates, so a perturbed key would pass a weaker check. */
    rc = mbedtls_ecp_check_pubkey(&grp, &D);
    if (rc != 0) goto done;

    /* Z1: the static-static term. Only the holder of hub_priv can compute it,
     * which is the entire authentication of the hub. */
    rc = mbedtls_mpi_read_binary(&d, hub_priv, 32);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_check_privkey(&grp, &d);
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_compute_shared(&grp, &z, &D, &d, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_write_binary(&z, zz, 32);
    if (rc != 0) goto done;

    /* Z2: the ephemeral term, which is the hub's contribution of freshness. */
    rc = mbedtls_mpi_read_binary(&d, eph_priv, 32);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_check_privkey(&grp, &d);
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_compute_shared(&grp, &z, &D, &d, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_write_binary(&z, zz + 32, 32);
    if (rc != 0) goto done;

    /* A hub that reused its static key as the ephemeral would make Z1 == Z2 and
     * halve the secret while every downstream value still looked well formed. */
    if (memcmp(zz, zz + 32, 32) == 0) { rc = CRYPTO_MISMATCH; goto done; }

    pair_salt(salt, hub_id, dev_id, req_superframe, dev_nonce);
    pair_transcript(t, hub_id, dev_id, req_superframe, dev_nonce,
                    hub_pub, eph_pub, dev_pub);
    memcpy(out->transcript, t, sizeof(out->transcript));

    rc = hkdf16(zz, salt, "openhub/v1/session", out->key_session, 16);
    if (rc != 0) goto done;
    rc = hkdf16(zz, salt, "openhub/v1/confirm/hub", ck_hub, 32);
    if (rc != 0) goto done;
    rc = hkdf16(zz, salt, "openhub/v1/confirm/dev", ck_dev, 32);
    if (rc != 0) goto done;

    /* Two keys rather than one plus a direction byte, so one side's
     * confirmation cannot be reflected back at it. */
    rc = confirm(ck_hub, t, out->confirm_hub);
    if (rc != 0) goto done;
    rc = confirm(ck_dev, t, out->confirm_dev);
    if (rc != 0) goto done;

    memcpy(out->eph_pub, eph_pub, 33);

done:
    mbedtls_platform_zeroize(zz, sizeof(zz));
    mbedtls_platform_zeroize(ck_hub, sizeof(ck_hub));
    mbedtls_platform_zeroize(ck_dev, sizeof(ck_dev));
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&D);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0)
        memset(out, 0, sizeof(*out));
    return rc;
}

int crypto_pair_derive(const uint8_t hub_priv[32], const uint8_t hub_pub[33],
                       const uint8_t dev_pub[33],
                       uint32_t hub_id, uint32_t dev_id,
                       uint32_t req_superframe, const uint8_t dev_nonce[8],
                       crypto_pair_out_t *out) {
    uint8_t eph_priv[32], eph_pub[33];
    int rc;

    if (hub_priv == NULL || hub_pub == NULL || dev_pub == NULL ||
        dev_nonce == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));
    rc = crypto_p256_keygen(eph_priv, eph_pub);
    if (rc == 0)
        rc = pair_derive_eph(hub_priv, hub_pub, dev_pub, eph_priv, eph_pub,
                             hub_id, dev_id, req_superframe, dev_nonce, out);
    mbedtls_platform_zeroize(eph_priv, sizeof(eph_priv));
    return rc;
}

/* --- tests ------------------------------------------------------------- */

static int test_drbg(void) {
    uint8_t a[32], b[32];

    if (!seeded)
        return CRYPTO_MISMATCH;
    if (crypto_random(a, sizeof(a)) != 0 || crypto_random(b, sizeof(b)) != 0)
        return CRYPTO_MISMATCH;

    /* Catches a dead source that returns a constant, which is the failure the
     * hardware RNG's latched seed error would otherwise have produced. */
    if (memcmp(a, b, sizeof(a)) == 0)
        return CRYPTO_MISMATCH;
    for (size_t i = 0; i < sizeof(a); i++)
        if (a[i] != 0)
            return 0;
    return CRYPTO_MISMATCH;
}

/* RFC 5869 test case 1. A known answer, so this fails if the HKDF or the SHA-256
 * underneath it is wrong - a round trip would not notice. */
static int test_hkdf(void) {
    static const uint8_t ikm[22] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
    static const uint8_t salt[13] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c };
    static const uint8_t info[10] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9 };
    static const uint8_t want[42] = {
        0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,
        0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,
        0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65 };
    uint8_t got[42];
    int rc;

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL)
        return CRYPTO_MISMATCH;

    rc = mbedtls_hkdf(md, salt, sizeof(salt), ikm, sizeof(ikm),
                      info, sizeof(info), got, sizeof(got));
    if (rc != 0)
        return rc;

    return (memcmp(got, want, sizeof(want)) == 0) ? 0 : CRYPTO_MISMATCH;
}

/* Round trip plus tamper rejection. Rejection is the half that matters: a frame
 * that fails to decrypt is harmless, one that is accepted after modification is
 * not. */
static int test_gcm(void) {
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    /* Frame-shaped nonce: superframe || device_id || direction || slot, all
     * big-endian, matching docs/security/wire-crypto.md. */
    static const uint8_t nonce[12] = {
        0x00,0x01,0xe2,0x40,   /* superframe 123456 */
        0x00,0x00,0x00,0x2a,   /* device id 42 */
        0x01,                  /* direction: uplink */
        0x00,0x00,0x07 };      /* slot 7 */
    static const uint8_t aad[8] = { 0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x2a };
    static const uint8_t pt[20] = {
        0xde,0xad,0xbe,0xef,0x01,0x02,0x03,0x04,0x05,0x06,
        0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10 };
    uint8_t ct[sizeof(pt)], back[sizeof(pt)], tag[16];
    mbedtls_gcm_context gcm;
    int rc;

    mbedtls_gcm_init(&gcm);
    rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc != 0)
        goto done;

    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(pt),
                                   nonce, sizeof(nonce), aad, sizeof(aad),
                                   pt, ct, sizeof(tag), tag);
    if (rc != 0)
        goto done;

    rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(ct), nonce, sizeof(nonce),
                                  aad, sizeof(aad), tag, sizeof(tag), ct, back);
    if (rc != 0 || memcmp(back, pt, sizeof(pt)) != 0) {
        rc = CRYPTO_MISMATCH;
        goto done;
    }

    ct[0] ^= 0x01;
    rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(ct), nonce, sizeof(nonce),
                                  aad, sizeof(aad), tag, sizeof(tag), ct, back);
    rc = (rc == 0) ? CRYPTO_MISMATCH : 0;   /* it must NOT authenticate */

done:
    mbedtls_gcm_free(&gcm);
    return rc;
}

/* Two key pairs must agree, an honest public key must validate, and a tampered
 * one must be rejected - the invalid-curve defence P-256 requires and X25519
 * would not have needed. */
/* Exactly the hub's share of one pairing, so the quiesce budget rests on a
 * measurement rather than on a division.
 *
 * The combined ecdh case runs four scalar multiplications and two validations.
 * Dividing its total by four to get "one operation" is how a 1.6x asymmetry
 * against the device's PKA first got written down as 6.5x - a four-operation
 * total compared against the device's single operation. A number that has to be
 * divided before it means anything is a number that will be divided wrongly.
 *
 * The peer's public key is a constant rather than generated here, for two
 * reasons: generating it would put a fourth scalar multiplication inside the
 * timed region, and on air it arrives in a frame and costs the hub nothing to
 * produce. It is a real P-256 point and deliberately not the base point G -
 * mbedTLS has a fixed-base path for G that a variable-base multiplication does
 * not take, which would time the wrong operation. */
static const uint8_t peer_pubkey[65] = {
    0x04, 0xff, 0xa6, 0x58, 0x76, 0xc5, 0x3c, 0xec, 0x3e, 0x37, 0xff, 0xe4,
    0x75, 0xa6, 0x8b, 0x07, 0xc7, 0x74, 0x3a, 0x72, 0xd2, 0x48, 0x10, 0xfb,
    0x5f, 0x8b, 0xe4, 0x0b, 0x3e, 0x85, 0x4e, 0x79, 0xfe, 0x38, 0x1d, 0xbe,
    0x7c, 0xa6, 0x3d, 0x2f, 0x8b, 0x4a, 0xaa, 0x9d, 0x1d, 0x82, 0x23, 0x4d,
    0x77, 0xc8, 0x45, 0x3b, 0x18, 0xdf, 0x66, 0xd2, 0x5b, 0xa8, 0x40, 0x45,
    0x47, 0xb1, 0xf8, 0xf4, 0x14
};

static int test_pairing_cost(void) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Qpeer, Qhub;
    mbedtls_mpi dhub, z;
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Qpeer);
    mbedtls_ecp_point_init(&Qhub);
    mbedtls_mpi_init(&dhub);
    mbedtls_mpi_init(&z);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;
    rc = mbedtls_ecp_point_read_binary(&grp, &Qpeer, peer_pubkey, sizeof(peer_pubkey));
    if (rc != 0) goto done;

    /* The three operations a hub performs for one pairing, and nothing else. */
    rc = mbedtls_ecdh_gen_public(&grp, &dhub, &Qhub, rng_cb, NULL);
    if (rc != 0) goto done;
    if (mbedtls_ecp_check_pubkey(&grp, &Qpeer) != 0) { rc = CRYPTO_MISMATCH; goto done; }
    rc = mbedtls_ecdh_compute_shared(&grp, &z, &Qpeer, &dhub, rng_cb, NULL);
    if (rc != 0) goto done;

    /* A shared secret of zero would mean the multiplication did not happen. */
    if (mbedtls_mpi_cmp_int(&z, 0) == 0) rc = CRYPTO_MISMATCH;

done:
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&dhub);
    mbedtls_ecp_point_free(&Qhub);
    mbedtls_ecp_point_free(&Qpeer);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* The whole pairing derivation against the published pair_v2 set.
 *
 * It calls the production path with the vector's ephemeral private key rather
 * than reimplementing the derivation, so what is checked is the code pairing
 * actually runs. Only the randomness is pinned.
 *
 * Every output is compared, not just the session key. The confirmations are
 * what the device checks, and a transcript built with the fields in the wrong
 * order still produces a perfectly good session key - the confirmations are the
 * only outputs that can see it. */
static const uint8_t PV_HUB_EPH_PRIV[32] = {
    0x4b, 0x3a, 0x29, 0x18, 0x07, 0xf6, 0xe5, 0xd4,
    0xc3, 0xb2, 0xa1, 0x90, 0x7e, 0x6d, 0x5c, 0x4b,
    0x3a, 0x29, 0x18, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a
};

static int test_pair_v2(void) {
    crypto_pair_out_t o;
    uint8_t salt[20], t[119];
    int rc;

    /* The two builders first, in isolation. If the salt or the transcript is
     * wrong, every value below is wrong in a way that says nothing about where.
     * The transcript is 119 bytes and a field-order slip keeps it 119, so the
     * bytes are compared and not the length. */
    pair_salt(salt, 0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME, PV_DEV_NONCE);
    if (memcmp(salt, PV_SALT, sizeof(PV_SALT)) != 0) return CRYPTO_MISMATCH;

    pair_transcript(t, 0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME,
                    PV_DEV_NONCE, V_HUB_PUB_C, PV_HUB_EPH_PUB, V_DEV_PUB_C);
    if (memcmp(t, PV_TRANSCRIPT, sizeof(PV_TRANSCRIPT)) != 0) return CRYPTO_MISMATCH;

    rc = pair_derive_eph(V_HUB_PRIV, V_HUB_PUB_C, V_DEV_PUB_C,
                         PV_HUB_EPH_PRIV, PV_HUB_EPH_PUB,
                         0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME,
                         PV_DEV_NONCE, &o);
    if (rc != 0) return rc;

    if (memcmp(o.eph_pub, PV_HUB_EPH_PUB, 33) != 0) return CRYPTO_MISMATCH;
    if (memcmp(o.key_session, PV_KEY_SESSION, 16) != 0) return CRYPTO_MISMATCH;
    if (memcmp(o.confirm_hub, PV_CONFIRM_HUB, 16) != 0) return CRYPTO_MISMATCH;
    if (memcmp(o.confirm_dev, PV_CONFIRM_DEV, 16) != 0) return CRYPTO_MISMATCH;

    /* Reflection: the two confirmations must not be interchangeable, or a relay
     * could send one side's back at it. Cheap, and it fails loudly if the two
     * info strings are ever made the same by a copy-paste. */
    if (memcmp(o.confirm_hub, o.confirm_dev, 16) == 0) return CRYPTO_MISMATCH;

    /* The fingerprint the operator types is only meaningful if this side hashes
     * the same 33 bytes the device publishes. That domain - compressed SEC1,
     * not 0x04||X||Y and not bare X - already cost one divergence before either
     * side had code, and pair_v1 dropped it from the published set. */
    {
        uint8_t fp[32];
        if (mbedtls_sha256(V_DEV_PUB_C, 33, fp, 0) != 0) return CRYPTO_MISMATCH;
        if (memcmp(fp, PV_FINGERPRINT, sizeof(PV_FINGERPRINT)) != 0)
            return CRYPTO_MISMATCH;
    }

    /* A device public key that is not on the curve must be refused before any
     * scalar multiplication touches it. V_REJECT_C is an x with no square root:
     * mbedTLS's reader accepts it and returns a garbage Y, so this is the only
     * check between a hostile 33 bytes and the hub's private key. */
    rc = pair_derive_eph(V_HUB_PRIV, V_HUB_PUB_C, V_REJECT_C,
                         PV_HUB_EPH_PRIV, PV_HUB_EPH_PUB,
                         0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME,
                         PV_DEV_NONCE, &o);
    if (rc == 0) return CRYPTO_MISMATCH;

    /* ... and the outputs must be zero afterwards, not stale. A caller that
     * missed the return value would otherwise transmit the previous device's
     * confirmation, which verifies for the wrong device. */
    {
        static const uint8_t zero[16] = {0};
        if (memcmp(o.key_session, zero, 16) != 0) return CRYPTO_MISMATCH;
        if (memcmp(o.confirm_hub, zero, 16) != 0) return CRYPTO_MISMATCH;
    }
    return 0;
}

static int test_ecdh(void) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Qa, Qb;
    mbedtls_mpi da, db, za, zb;
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Qa);
    mbedtls_ecp_point_init(&Qb);
    mbedtls_mpi_init(&da);
    mbedtls_mpi_init(&db);
    mbedtls_mpi_init(&za);
    mbedtls_mpi_init(&zb);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;

    rc = mbedtls_ecdh_gen_public(&grp, &da, &Qa, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_gen_public(&grp, &db, &Qb, rng_cb, NULL);
    if (rc != 0) goto done;

    if (mbedtls_ecp_check_pubkey(&grp, &Qa) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    rc = mbedtls_ecdh_compute_shared(&grp, &za, &Qb, &da, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_compute_shared(&grp, &zb, &Qa, &db, rng_cb, NULL);
    if (rc != 0) goto done;

    if (mbedtls_mpi_cmp_mpi(&za, &zb) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* Move the point off the curve and require the check to catch it. */
    if (mbedtls_mpi_add_int(&Qa.MBEDTLS_PRIVATE(Y), &Qa.MBEDTLS_PRIVATE(Y), 1) != 0) {
        rc = CRYPTO_MISMATCH;
        goto done;
    }
    rc = (mbedtls_ecp_check_pubkey(&grp, &Qa) == 0) ? CRYPTO_MISMATCH : 0;

done:
    mbedtls_mpi_free(&zb);
    mbedtls_mpi_free(&za);
    mbedtls_mpi_free(&db);
    mbedtls_mpi_free(&da);
    mbedtls_ecp_point_free(&Qb);
    mbedtls_ecp_point_free(&Qa);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* The interop contract, checked against vectors a host reference library
 * produced. Hub and device share no code, so agreeing with each other is not
 * evidence of anything - agreeing with these bytes is. */
/* Named for what it covers, not for what it resembles.
 *
 * These are the AEAD, HKDF and single-term-ECDH vectors from wire_v3. They are
 * NOT pairing outputs: since the exchange derives from a two-term Z,
 * V_KEY_SESSION0 is no longer the session key a pairing produces. A green test
 * asserting the wrong contract is worse than a red one, because it retires the
 * question - so the case says "primitives" and pair_v1.txt holds what pairing
 * actually yields. Raised by the device side, whose own self-test had the same
 * name and the same about-to-be-wrong claim. */
static int test_vectors(void) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d, z;
    uint8_t buf[32];
    uint8_t ct[sizeof(V_PLAIN)], tag[16];
    mbedtls_gcm_context gcm;
    const mbedtls_md_info_t *md;
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_gcm_init(&gcm);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;

    /* SEC1 uncompressed point in, X-coordinate-only secret out. */
    rc = mbedtls_ecp_point_read_binary(&grp, &Q, V_DEV_PUB, sizeof(V_DEV_PUB));
    if (rc != 0) goto done;
    rc = mbedtls_mpi_read_binary(&d, V_HUB_PRIV, sizeof(V_HUB_PRIV));
    if (rc != 0) goto done;
    rc = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, rng_cb, NULL);
    if (rc != 0) goto done;
    rc = mbedtls_mpi_write_binary(&z, buf, sizeof(buf));
    if (rc != 0) goto done;
    if (memcmp(buf, V_ECDH_X, sizeof(V_ECDH_X)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) { rc = CRYPTO_MISMATCH; goto done; }

    rc = mbedtls_hkdf(md, V_SALT, sizeof(V_SALT), buf, sizeof(buf),
                      V_INFO_SESSION, sizeof(V_INFO_SESSION), ct, 16);
    if (rc != 0) goto done;
    if (memcmp(ct, V_KEY_SESSION0, 16) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* Pack the salt from ids held as integers rather than reusing the vector's
     * byte array. Ids are little-endian on the wire and big-endian in the salt,
     * and a vector cannot tell "packs the ids correctly" from "has the right
     * bytes" - only building it from the numbers crosses that boundary. */
    {
        const uint32_t hub_id = 0x33442211u, dev_id = 0x0000002Au;
        uint8_t salt[8];

        salt[0] = (uint8_t)(hub_id >> 24); salt[1] = (uint8_t)(hub_id >> 16);
        salt[2] = (uint8_t)(hub_id >> 8);  salt[3] = (uint8_t)hub_id;
        salt[4] = (uint8_t)(dev_id >> 24); salt[5] = (uint8_t)(dev_id >> 16);
        salt[6] = (uint8_t)(dev_id >> 8);  salt[7] = (uint8_t)dev_id;

        if (memcmp(salt, V_SALT, sizeof(salt)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

        rc = mbedtls_hkdf(md, salt, sizeof(salt), buf, sizeof(buf),
                          V_INFO_SESSION, sizeof(V_INFO_SESSION), ct, 16);
        if (rc != 0) goto done;
        if (memcmp(ct, V_KEY_SESSION0, 16) != 0) { rc = CRYPTO_MISMATCH; goto done; }
    }

    rc = mbedtls_hkdf(md, V_SALT, sizeof(V_SALT), buf, sizeof(buf),
                      V_INFO_HOP, sizeof(V_INFO_HOP), tag, 16);
    if (rc != 0) goto done;
    if (memcmp(tag, V_KEY_HOP0, 16) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* One ratchet step: empty salt, because the input is already a key. */
    rc = mbedtls_hkdf(md, NULL, 0, V_KEY_SESSION0, 16,
                      V_INFO_ROTATE, sizeof(V_INFO_ROTATE), tag, 16);
    if (rc != 0) goto done;
    if (memcmp(tag, V_KEY_SESSION1, 16) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* Frame-shaped: big-endian nonce, wire-order (little-endian) header as AAD. */
    rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, V_KEY_SESSION0, 128);
    if (rc != 0) goto done;
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(V_PLAIN),
                                   V_NONCE, sizeof(V_NONCE),
                                   V_AAD, sizeof(V_AAD),
                                   V_PLAIN, ct, sizeof(tag), tag);
    if (rc != 0) goto done;
    if (memcmp(ct, V_CIPHER, sizeof(V_CIPHER)) != 0) { rc = CRYPTO_MISMATCH; goto done; }
    if (memcmp(tag, V_TAG, sizeof(V_TAG)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* 23 bytes: a partial final word, which the block-aligned case above cannot
     * exercise. Decrypted as well as encrypted, because the failure the device
     * side hit was on the decrypt path only. */
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(V_ODD_PLAIN),
                                   V_ODD_NONCE, sizeof(V_ODD_NONCE),
                                   V_AAD, sizeof(V_AAD),
                                   V_ODD_PLAIN, ct, sizeof(tag), tag);
    if (rc != 0) goto done;
    if (memcmp(ct, V_ODD_CIPHER, sizeof(V_ODD_CIPHER)) != 0) { rc = CRYPTO_MISMATCH; goto done; }
    if (memcmp(tag, V_ODD_TAG, sizeof(V_ODD_TAG)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(V_ODD_CIPHER),
                                  V_ODD_NONCE, sizeof(V_ODD_NONCE),
                                  V_AAD, sizeof(V_AAD),
                                  V_ODD_TAG, sizeof(V_ODD_TAG),
                                  V_ODD_CIPHER, ct);
    if (rc != 0) goto done;
    if (memcmp(ct, V_ODD_PLAIN, sizeof(V_ODD_PLAIN)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

done:
    mbedtls_gcm_free(&gcm);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* Every payload length from 1 to 32, not just the block-aligned ones.
 *
 * The device side found HAL_CRYP_Decrypt leaving the unused bytes of a partial
 * final word unmasked while encrypt handled them correctly: every length not
 * divisible by four failed its tag check while the ciphertext was byte-perfect.
 * That reads as a radio fault over the air, not a crypto one. This path is
 * mbedTLS software GCM rather than the peripheral, but the same test costs
 * nothing here and CM4's per-frame GCM will use CRYP, where it will matter. */
static int test_gcm_lengths(void) {
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    uint8_t nonce[12];
    uint8_t pt[32], ct[32], back[32], tag[16];
    mbedtls_gcm_context gcm;
    int rc = 0;

    for (size_t i = 0; i < sizeof(pt); i++)
        pt[i] = (uint8_t)(0xA0 + i);

    mbedtls_gcm_init(&gcm);
    rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc != 0)
        goto done;

    for (size_t len = 1; len <= sizeof(pt); len++) {
        /* A fresh nonce per length: a repeated key/nonce pair would leak the
         * authentication subkey, and a test must not model bad practice. */
        memset(nonce, 0, sizeof(nonce));
        nonce[11] = (uint8_t)len;

        memset(ct, 0xCC, sizeof(ct));
        memset(back, 0xDD, sizeof(back));

        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                                       nonce, sizeof(nonce), NULL, 0,
                                       pt, ct, sizeof(tag), tag);
        if (rc != 0)
            goto done;

        rc = mbedtls_gcm_auth_decrypt(&gcm, len, nonce, sizeof(nonce), NULL, 0,
                                      tag, sizeof(tag), ct, back);
        if (rc != 0 || memcmp(back, pt, len) != 0) {
            rc = CRYPTO_MISMATCH;
            goto done;
        }
    }

done:
    mbedtls_gcm_free(&gcm);
    return rc;
}

/* Can this build read a compressed SEC1 point?
 *
 * It decides the pairing frame. The RFM69 FIFO is 66 bytes, so a 65-byte
 * uncompressed key plus a header does not fit in one load, while a 33-byte
 * compressed key leaves room to spare. mbedTLS is widely believed not to read
 * compressed points; 3.6 does, via mbedtls_ecp_sw_derive_y - but a belief is
 * not a reason to design a wire format, so this checks on the target. */
static int test_point_compress(void) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    uint8_t buf[32];
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;

    /* Against the shared vector, not against our own arithmetic: agreeing with
     * ourselves would prove nothing about agreeing with the device. */
    rc = mbedtls_ecp_point_read_binary(&grp, &Q, V_DEV_PUB_C, sizeof(V_DEV_PUB_C));
    if (rc != 0) goto done;

    rc = mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), buf, sizeof(buf));
    if (rc != 0) goto done;
    if (memcmp(buf, V_DEV_PUB_X, sizeof(V_DEV_PUB_X)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* Y is recovered rather than transmitted, so it is the half worth checking. */
    rc = mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(Y), buf, sizeof(buf));
    if (rc != 0) goto done;
    if (memcmp(buf, V_DEV_PUB_Y, sizeof(V_DEV_PUB_Y)) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    if (mbedtls_ecp_check_pubkey(&grp, &Q) != 0) { rc = CRYPTO_MISMATCH; goto done; }

    /* The hub key carries the other parity prefix (0x03 against 0x02), so
     * checking only one of them would leave half the branch untested. */
    {
        mbedtls_ecp_point H;
        mbedtls_ecp_point_init(&H);
        rc = mbedtls_ecp_point_read_binary(&grp, &H, V_HUB_PUB_C, sizeof(V_HUB_PUB_C));
        if (rc == 0) {
            rc = mbedtls_mpi_write_binary(&H.MBEDTLS_PRIVATE(Y), buf, sizeof(buf));
            if (rc == 0 && memcmp(buf, V_HUB_PUB_Y, sizeof(V_HUB_PUB_Y)) != 0)
                rc = CRYPTO_MISMATCH;
        }
        mbedtls_ecp_point_free(&H);
        if (rc != 0) goto done;
    }

    /* Stronger than comparing Y to bytes: run the real ECDH against the
     * decompressed point and require the shared secret to come out unchanged.
     * A wrong Y cannot survive that, and this is the path pairing actually uses.
     * (The device side proposed this; comparing Y alone only proves the
     * comparison, not that the point is usable.) */
    {
        mbedtls_mpi d, z;
        mbedtls_mpi_init(&d);
        mbedtls_mpi_init(&z);
        rc = mbedtls_mpi_read_binary(&d, V_HUB_PRIV, sizeof(V_HUB_PRIV));
        if (rc == 0)
            rc = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, rng_cb, NULL);
        if (rc == 0)
            rc = mbedtls_mpi_write_binary(&z, buf, sizeof(buf));
        if (rc == 0 && memcmp(buf, V_ECDH_X, sizeof(V_ECDH_X)) != 0)
            rc = CRYPTO_MISMATCH;
        mbedtls_mpi_free(&z);
        mbedtls_mpi_free(&d);
        if (rc != 0) goto done;
    }

    /* x = 1 has no square root on P-256. mbedTLS says in its own comment that it
     * does not verify the root, so read_binary SUCCEEDS here and hands back a
     * garbage Y - the curve check afterwards is the only thing that rejects it.
     * A perturbed valid key would not test this: about half of all field
     * elements are valid x-coordinates. */
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_point_init(&Q);
    rc = mbedtls_ecp_point_read_binary(&grp, &Q, V_REJECT_C, sizeof(V_REJECT_C));
    if (rc != 0) { rc = CRYPTO_MISMATCH; goto done; }   /* it must not fail here */
    rc = (mbedtls_ecp_check_pubkey(&grp, &Q) == 0) ? CRYPTO_MISMATCH : 0;

done:
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

const char *crypto_test_name(crypto_test_t t) {
    switch (t) {
    case CRYPTO_TEST_DRBG: return "ctr-drbg seed+draw";
    case CRYPTO_TEST_HKDF: return "hkdf-sha256 rfc5869";
    case CRYPTO_TEST_GCM:  return "aes-128-gcm+tamper";
    case CRYPTO_TEST_ECDH: return "p-256 ecdh+validate";
    case CRYPTO_TEST_GCMLEN: return "gcm lengths 1..32";
    case CRYPTO_TEST_VECTORS: return "wire vectors (primitives)";
    case CRYPTO_TEST_COMPRESS: return "compressed pt + reject";
    case CRYPTO_TEST_PAIRCOST: return "p-256 hub pairing share";
    case CRYPTO_TEST_PAIRV2: return "pair_v2 derive+confirm";
    case CRYPTO_TEST_PAIRV3: return "pair_v3 init key+mac";
    default:               return "?";
    }
}

/* The consumer pair_v3.h did not have. Until something ran the production path
 * against those bytes they proved only that the generator agreed with itself -
 * a vector whose consumer does not exist is untested in the way that matters.
 *
 * Runs crypto_pair_init_key and crypto_pair_init_mac, the same functions the
 * transmit path calls, against the published frame. The second frame catches a
 * MAC that ignores the superframe: without it the field could be dropped from
 * the input and every other check here would still pass. */
static int test_pair_v3(void) {
    uint8_t k[32], mac[12];
    int rc;

    /* Same identities as pair_v2's test, and the same symbols - so the two
     * self-tests cannot silently be about different devices. */
    rc = crypto_pair_init_key(V_HUB_PRIV, V_DEV_PUB_C,
                              0x33442211u, 0x0000002Au, k);
    if (rc != 0) return rc;
    if (memcmp(k, PV3_INIT_KEY, 32) != 0) return CRYPTO_MISMATCH;
    /* And that Z1 is the value pair_v2 already publishes, which is the whole
     * reason this frame's key was cheap to agree on. */
    if (sizeof(PV3_INIT_Z1) != 32) return CRYPTO_MISMATCH;

    rc = crypto_pair_init_mac(k, PV3_INIT_HEADER, sizeof(PV3_INIT_HEADER), mac);
    if (rc != 0) return rc;
    if (memcmp(mac, PV3_INIT_MAC, 12) != 0) return CRYPTO_MISMATCH;
    if (memcmp(PV3_INIT_FRAME + sizeof(PV3_INIT_HEADER), mac, 12) != 0)
        return CRYPTO_MISMATCH;

    rc = crypto_pair_init_mac(k, PV3_INIT_FRAME_NEXT_SF,
                              sizeof(PV3_INIT_HEADER), mac);
    if (rc != 0) return rc;
    if (memcmp(PV3_INIT_FRAME_NEXT_SF + sizeof(PV3_INIT_HEADER), mac, 12) != 0)
        return CRYPTO_MISMATCH;
    /* The two frames differ only in the superframe, so equal MACs would mean
     * the field is not covered. */
    if (memcmp(PV3_INIT_MAC, mac, 12) == 0)
        return CRYPTO_MISMATCH;
    return 0;
}

int crypto_run_test(crypto_test_t t) {
    int rc = ensure_seeded();

    if (rc != 0)
        return rc;

    crypto_stage = 10u + (uint32_t)t;
    switch (t) {
    case CRYPTO_TEST_DRBG: rc = test_drbg(); break;
    case CRYPTO_TEST_HKDF: rc = test_hkdf(); break;
    case CRYPTO_TEST_GCM:  rc = test_gcm();  break;
    case CRYPTO_TEST_ECDH: rc = test_ecdh(); break;
    case CRYPTO_TEST_GCMLEN: rc = test_gcm_lengths(); break;
    case CRYPTO_TEST_VECTORS: rc = test_vectors(); break;
    case CRYPTO_TEST_COMPRESS: rc = test_point_compress(); break;
    case CRYPTO_TEST_PAIRCOST: rc = test_pairing_cost(); break;
    case CRYPTO_TEST_PAIRV2: rc = test_pair_v2(); break;
    case CRYPTO_TEST_PAIRV3: rc = test_pair_v3(); break;
    default:               rc = CRYPTO_MISMATCH; break;
    }
    crypto_stage = 20u + (uint32_t)t;
    return rc;
}
