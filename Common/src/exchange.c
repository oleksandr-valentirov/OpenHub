/* The key schedule. Big-endian into hashes, little-endian on the wire.
 * radio_devices_docs/wl55_device/security/self-tests.md */
#include <string.h>

#include "exchange.h"

static const uint8_t info_session[]     = "openhub/v1/session";
static const uint8_t info_confirm_hub[] = "openhub/v1/confirm/hub";
static const uint8_t info_confirm_dev[] = "openhub/v1/confirm/dev";

static uint8_t *put_be32(uint8_t *p, uint32_t v) {
    *p++ = (uint8_t)(v >> 24);
    *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

void exchange_salt(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                   const uint8_t *dev_nonce, uint8_t *out) {
    uint8_t *p = put_be32(put_be32(put_be32(out, hub_id), dev_id), req_superframe);
    memcpy(p, dev_nonce, EXCHANGE_NONCE_LEN);
}

void exchange_transcript(uint32_t hub_id, uint32_t dev_id, uint32_t req_superframe,
                         const uint8_t *dev_nonce,
                         const uint8_t *hub_static_c,
                         const uint8_t *hub_eph_c,
                         const uint8_t *dev_static_c,
                         uint8_t *out) {
    uint8_t *p = out + EXCHANGE_SALT_LEN;
    exchange_salt(hub_id, dev_id, req_superframe, dev_nonce, out);
    memcpy(p, hub_static_c, EXCHANGE_POINT_LEN);  p += EXCHANGE_POINT_LEN;
    memcpy(p, hub_eph_c,    EXCHANGE_POINT_LEN);  p += EXCHANGE_POINT_LEN;
    memcpy(p, dev_static_c, EXCHANGE_POINT_LEN);
}

int exchange_derive(const uint8_t *z1, const uint8_t *z2,
                    const uint8_t *salt, const uint8_t *transcript,
                    exchange_keys_t *out) {
    uint8_t z[EXCHANGE_Z_LEN];
    uint8_t mac[CRYPTO_SHA256_LEN];
    int rc;

    /* Fixed 32-byte terms, so the concatenation carries no length ambiguity. */
    memcpy(z, z1, EXCHANGE_Z_TERM_LEN);
    memcpy(z + EXCHANGE_Z_TERM_LEN, z2, EXCHANGE_Z_TERM_LEN);

    rc = crypto_hkdf_sha256(salt, EXCHANGE_SALT_LEN, z, sizeof(z),
                            info_session, sizeof(info_session) - 1u,
                            out->session, EXCHANGE_KEY_LEN);
    if (rc == 0)
        rc = crypto_hkdf_sha256(salt, EXCHANGE_SALT_LEN, z, sizeof(z),
                                info_confirm_hub, sizeof(info_confirm_hub) - 1u,
                                out->confirm_key_hub, EXCHANGE_CONFIRM_KEY_LEN);
    if (rc == 0)
        rc = crypto_hkdf_sha256(salt, EXCHANGE_SALT_LEN, z, sizeof(z),
                                info_confirm_dev, sizeof(info_confirm_dev) - 1u,
                                out->confirm_key_dev, EXCHANGE_CONFIRM_KEY_LEN);

    /* HMAC over the transcript itself: hashing first cannot name the field.
     * radio_devices_docs/wl55_device/testing/host-tests.md */
    if (rc == 0)
        rc = crypto_hmac_sha256(out->confirm_key_hub, EXCHANGE_CONFIRM_KEY_LEN,
                                transcript, EXCHANGE_TRANSCRIPT_LEN, mac);
    if (rc == 0)
        memcpy(out->confirm_hub, mac, EXCHANGE_CONFIRM_LEN);
    if (rc == 0)
        rc = crypto_hmac_sha256(out->confirm_key_dev, EXCHANGE_CONFIRM_KEY_LEN,
                                transcript, EXCHANGE_TRANSCRIPT_LEN, mac);
    if (rc == 0)
        memcpy(out->confirm_dev, mac, EXCHANGE_CONFIRM_LEN);

    memset(z, 0, sizeof(z));
    memset(mac, 0, sizeof(mac));

    /* A half-filled key set is the one output nobody may act on. */
    if (rc != 0)
        memset(out, 0, sizeof(*out));
    return rc;
}

int exchange_confirm_equal(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < EXCHANGE_CONFIRM_LEN; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int exchange_eph_is_static(const uint8_t *hub_eph_c, const uint8_t *hub_static_c) {
    return memcmp(hub_eph_c, hub_static_c, EXCHANGE_POINT_LEN) == 0;
}
