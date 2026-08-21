/**
 * @file crypto.c
 * @brief Asymmetric and key-derivation crypto, and the self-tests over it. ADR-0011
 *
 * radio_devices_docs/open_hub/security/self-tests.md
 */
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
#include "pair_prov.h"
#include "radio_protocol.h"

#define CRYPTO_MISMATCH (-1)

static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;
static int seeded = 0;

/* Readable over SWD, since the CLI flushes only after a handler returns. */
volatile uint32_t crypto_stage = 0;

/* Personalisation, which separates two DRBGs and is not secret. */
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

/* Seeded on first use, never at boot.
 * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* rng_cb reaches the guarded hardware draw.
     * radio_devices_docs/open_hub/security/entropy.md */
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
    /* Rejects zero and anything at or above the group order.
     * radio_devices_docs/open_hub/security/self-tests.md */
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
 * radio_devices_docs/radio/crypto/wire-crypto.md */
static void pair_salt(uint8_t salt[20], uint32_t hub_id, uint32_t dev_id,
                      uint32_t req_superframe, const uint8_t dev_nonce[8]) {
    be32(salt, hub_id);
    be32(salt + 4, dev_id);
    be32(salt + 8, req_superframe);
    memcpy(salt + 12, dev_nonce, 8);
}

/* Both hub keys are bound; hub_static is never transmitted. */
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

/* pair_v3's invitation key: Z1 alone. Derive once per device. ADR-0021 */
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
    /* read_binary accepts an x with no square root; this is the only rejection.
     * radio_devices_docs/open_hub/security/self-tests.md */
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

/* HMAC-SHA256 truncated to 96 bits, over the frame's cleartext. ADR-0021 */
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

/* The ephemeral is supplied rather than drawn, so a self-test can pin only it.
 * radio_devices_docs/open_hub/security/self-tests.md */
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
     * radio_devices_docs/open_hub/security/self-tests.md */
    rc = mbedtls_ecp_check_pubkey(&grp, &D);
    if (rc != 0) goto done;

    /* Z1, the static-static term: the entire authentication of the hub. */
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

    /* Reusing the static key as the ephemeral would make Z1 == Z2.
     * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* Two keys, so one side's confirmation cannot be reflected back at it. */
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

    /* Catches a dead source returning a constant.
     * radio_devices_docs/open_hub/security/self-tests.md */
    if (memcmp(a, b, sizeof(a)) == 0)
        return CRYPTO_MISMATCH;
    for (size_t i = 0; i < sizeof(a); i++)
        if (a[i] != 0)
            return 0;
    return CRYPTO_MISMATCH;
}

/* RFC 5869 test case 1: a known answer, which a round trip would not be. */
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

/* Round trip plus tamper rejection, which is the half that matters.
 * radio_devices_docs/open_hub/security/self-tests.md */
static int test_gcm(void) {
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    /* Nonce per radio_devices_docs/radio/crypto/wire-crypto.md. */
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

/* Two key pairs agree, an honest key validates, a tampered one is rejected.
 * radio_devices_docs/open_hub/security/self-tests.md */
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
 * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* The two builders first, in isolation, and by bytes rather than by length.
     * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* Reflection: the two confirmations must not be interchangeable. */
    if (memcmp(o.confirm_hub, o.confirm_dev, 16) == 0) return CRYPTO_MISMATCH;

    /* The fingerprint domain: compressed SEC1, not 0x04||X||Y and not bare X.
     * radio_devices_docs/open_hub/security/self-tests.md */
    {
        uint8_t fp[32];
        if (mbedtls_sha256(V_DEV_PUB_C, 33, fp, 0) != 0) return CRYPTO_MISMATCH;
        if (memcmp(fp, PV_FINGERPRINT, sizeof(PV_FINGERPRINT)) != 0)
            return CRYPTO_MISMATCH;
    }

    /* A key not on the curve, refused before any scalar multiplication.
     * radio_devices_docs/open_hub/security/self-tests.md */
    rc = pair_derive_eph(V_HUB_PRIV, V_HUB_PUB_C, V_REJECT_C,
                         PV_HUB_EPH_PRIV, PV_HUB_EPH_PUB,
                         0x33442211u, 0x0000002Au, PAIR_REQ_SUPERFRAME,
                         PV_DEV_NONCE, &o);
    if (rc == 0) return CRYPTO_MISMATCH;

    /* ... and the outputs must be zero afterwards, never stale. */
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

/* wire_v3's primitives, not pairing outputs - the name is the coverage.
 * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* Packed from ids held as integers, never from the vector's byte array.
     * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* 23 bytes: a partial final word, decrypted as well as encrypted.
     * radio_devices_docs/open_hub/security/self-tests.md */
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
 * radio_devices_docs/open_hub/security/self-tests.md */
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
        /* A fresh nonce per length: a test must not model bad practice. */
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

/* Can this build read a compressed SEC1 point? It decides the wire format.
 * ADR-0018, radio_devices_docs/open_hub/security/self-tests.md */
static int test_point_compress(void) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    uint8_t buf[32];
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) goto done;

    /* Against the shared vector, never against our own arithmetic. */
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

    /* The hub key carries the other parity prefix, 0x03 against 0x02. */
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

    /* Real ECDH against the decompressed point, not a comparison of Y to bytes.
     * radio_devices_docs/open_hub/security/self-tests.md */
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

    /* x = 1 has no square root on P-256; a perturbed valid key would not test this.
     * radio_devices_docs/open_hub/security/self-tests.md */
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
    case CRYPTO_TEST_PAIRPROV: return "superframe provenance";
    default:               return "?";
    }
}

/* pair_prov's consumer: pair_v2 with only the superframe moved.
 * radio_devices_docs/open_hub/security/self-tests.md */
static int test_pair_prov(void) {
    crypto_pair_out_t o;
    int rc;

    rc = pair_derive_eph(V_HUB_PRIV, V_HUB_PUB_C, V_DEV_PUB_C,
                         PV_HUB_EPH_PRIV, PV_HUB_EPH_PUB,
                         0x33442211u, 0x0000002Au, PROV_REQ_SUPERFRAME,
                         PV_DEV_NONCE, &o);
    if (rc != 0) return rc;
    if (memcmp(o.transcript, PROV_TRANSCRIPT, sizeof(PROV_TRANSCRIPT)) != 0)
        return CRYPTO_MISMATCH;
    if (memcmp(o.key_session, PROV_KEY_SESSION, 16) != 0) return CRYPTO_MISMATCH;
    if (memcmp(o.confirm_hub, PROV_CONFIRM_HUB, 16) != 0) return CRYPTO_MISMATCH;
    if (memcmp(o.confirm_dev, PROV_CONFIRM_DEV, 16) != 0) return CRYPTO_MISMATCH;

    /* The decoy must produce the published wrong answer exactly.
     * radio_devices_docs/open_hub/security/self-tests.md */
    rc = pair_derive_eph(V_HUB_PRIV, V_HUB_PUB_C, V_DEV_PUB_C,
                         PV_HUB_EPH_PRIV, PV_HUB_EPH_PUB,
                         0x33442211u, 0x0000002Au, PROV_BEACON_SUPERFRAME,
                         PV_DEV_NONCE, &o);
    if (rc != 0) return rc;
    if (memcmp(o.confirm_hub, PROV_CONFIRM_HUB_IF_BEACON, 16) != 0)
        return CRYPTO_MISMATCH;
    if (memcmp(o.confirm_hub, PROV_CONFIRM_HUB, 16) == 0) return CRYPTO_MISMATCH;
    return 0;
}

/* The consumer pair_v3.h did not have, running the same functions the transmit
 * path calls. radio_devices_docs/open_hub/security/self-tests.md */
static int test_pair_v3(void) {
    uint8_t k[32], mac[12];
    int rc;

    /* The same symbols as pair_v2's test, not merely the same values. */
    rc = crypto_pair_init_key(V_HUB_PRIV, V_DEV_PUB_C,
                              0x33442211u, 0x0000002Au, k);
    if (rc != 0) return rc;
    if (memcmp(k, PV3_INIT_KEY, 32) != 0) return CRYPTO_MISMATCH;
    /* And that Z1 is the value pair_v2 already publishes. */
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
    /* The two frames differ only in the superframe; equal MACs would be a hole. */
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
    case CRYPTO_TEST_PAIRPROV: rc = test_pair_prov(); break;
    default:               rc = CRYPTO_MISMATCH; break;
    }
    crypto_stage = 20u + (uint32_t)t;
    return rc;
}
