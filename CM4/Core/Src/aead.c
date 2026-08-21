/**
 * @file aead.c
 * @brief AES-128-GCM through CRYP, stating its whole configuration on every call.
 *
 * radio_devices_docs/radio/crypto/wire-crypto.md
 */
#include <string.h>

#include "main.h"
#include "aead.h"
#include "radio_protocol.h"
#include "pair_v2.h"
#include "link_v5.h"

extern CRYP_HandleTypeDef hcryp;

#define CRYP_TIMEOUT_MS  50u

/* Bounds every sealed body; the assert names them, so no arithmetic here rots. */
#define AEAD_MAX_LEN  32u
_Static_assert(sizeof(radio_pair_grant_t)    <= AEAD_MAX_LEN &&
               sizeof(radio_uplink_report_t) <= AEAD_MAX_LEN &&
               sizeof(radio_downlink_cmd_t)  <= AEAD_MAX_LEN,
               "a sealed body is larger than the CRYP path's buffers");

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

void aead_nonce(uint8_t out[AEAD_NONCE_BYTES], uint32_t superframe,
                uint32_t dev_id, uint8_t direction, uint32_t slot) {
    be32(out, superframe);
    be32(out + 4, dev_id);
    out[8]  = direction;
    out[9]  = (uint8_t)(slot >> 16);
    out[10] = (uint8_t)(slot >> 8);
    out[11] = (uint8_t)slot;
}

/* CRYP's key and IV registers hold big-endian words, and DataType does not
 * reach them. radio_devices_docs/radio/crypto/wire-crypto.md */
static void words_be(uint32_t *w, const uint8_t *b, unsigned n_words) {
    for (unsigned i = 0; i < n_words; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
}

/* A 16-byte initial counter block: the nonce, then the GCM counter's 2. */
static void iv_from_nonce(uint32_t iv[4], const uint8_t nonce[AEAD_NONCE_BYTES]) {
    uint8_t b[16];

    memcpy(b, nonce, AEAD_NONCE_BYTES);
    b[12] = 0; b[13] = 0; b[14] = 0; b[15] = 2;
    words_be(iv, b, 4);
}

static int configure(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
                     const uint8_t *aad, size_t aad_len,
                     uint32_t iv[4], uint32_t key_words[4]) {
    CRYP_ConfigTypeDef cfg;

    words_be(key_words, key, AEAD_KEY_BYTES / 4u);
    iv_from_nonce(iv, nonce);

    if (HAL_CRYP_GetConfig(&hcryp, &cfg) != HAL_OK)
        return -1;
    cfg.Algorithm  = CRYP_AES_GCM;
    cfg.KeySize    = CRYP_KEYSIZE_128B;
    /* 8-bit, not the 32-bit the .ioc configures.
     * radio_devices_docs/radio/hopping.md */
    cfg.DataType   = CRYP_DATATYPE_8B;
    cfg.pKey       = key_words;
    cfg.pInitVect  = iv;
    cfg.Header     = (uint32_t *)(void *)aad;
    cfg.HeaderSize = (uint32_t)aad_len;
    cfg.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_BYTE;
    cfg.DataWidthUnit   = CRYP_DATAWIDTHUNIT_BYTE;
    if (HAL_CRYP_SetConfig(&hcryp, &cfg) != HAL_OK)
        return -1;
    return 0;
}

int aead_seal(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[AEAD_TAG_BYTES]) {
    uint32_t iv[4], key_words[4];
    uint8_t  in[AEAD_MAX_LEN];
    uint8_t  out[AEAD_MAX_LEN];

    if (len > sizeof(in))
        return -1;
    if (configure(key, nonce, aad, aad_len, iv, key_words) != 0)
        return -1;

    /* Zeroed before the copy, as in aead_open, so both paths define the tail. */
    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    if (len > 0u)
        memcpy(in, pt, len);

    if (HAL_CRYP_Encrypt(&hcryp, (uint32_t *)(void *)in, (uint16_t)len,
                         (uint32_t *)(void *)out, CRYP_TIMEOUT_MS) != HAL_OK)
        return -1;
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&hcryp, (uint32_t *)(void *)tag,
                                          CRYP_TIMEOUT_MS) != HAL_OK)
        return -1;
    if (len > 0u && ct != NULL)
        memcpy(ct, out, len);
    return 0;
}

int aead_open(const uint8_t key[AEAD_KEY_BYTES], const uint8_t nonce[AEAD_NONCE_BYTES],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *ct, size_t len, uint8_t *pt,
              const uint8_t tag[AEAD_TAG_BYTES]) {
    uint32_t iv[4], key_words[4];
    uint8_t  in[AEAD_MAX_LEN];
    uint8_t  out[AEAD_MAX_LEN];
    uint8_t  got[AEAD_TAG_BYTES];
    uint8_t  diff = 0;

    if (len > sizeof(in))
        return -1;
    if (configure(key, nonce, aad, aad_len, iv, key_words) != 0)
        return -1;

    /* HAL_CRYP_Decrypt does not mask a partial final word, while encrypt does.
     * ADR-0013, radio_devices_docs/open_hub/security/self-tests.md */
    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    if (len > 0u)
        memcpy(in, ct, len);

    if (HAL_CRYP_Decrypt(&hcryp, (uint32_t *)(void *)in, (uint16_t)len,
                         (uint32_t *)(void *)out, CRYP_TIMEOUT_MS) != HAL_OK)
        return -1;
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&hcryp, (uint32_t *)(void *)got,
                                          CRYP_TIMEOUT_MS) != HAL_OK)
        return -1;

    /* Constant time, and no plaintext is handed back until it verifies. */
    for (unsigned i = 0; i < AEAD_TAG_BYTES; i++)
        diff |= (uint8_t)(got[i] ^ tag[i]);
    if (diff != 0) {
        if (len > 0u && pt != NULL)
            memset(pt, 0, len);
        return -1;
    }
    if (len > 0u && pt != NULL)
        memcpy(pt, out, len);
    return 0;
}

int aead_selftest(void) {
    uint8_t buf[32];
    uint8_t tag[AEAD_TAG_BYTES];

    /* PAIR_ACCEPT: 19 bytes sealed, the length the decrypt defect fires on. */
    if (aead_seal(PV_KEY_SESSION, PV_ACCEPT_NONCE, PV_ACCEPT_AAD,
                  sizeof(PV_ACCEPT_AAD), PV_ACCEPT_PLAIN, sizeof(PV_ACCEPT_PLAIN),
                  buf, tag) != 0)
        return -1;
    if (memcmp(buf, PV_FRAME_ACCEPT + RADIO_PAIR_ACCEPT_AAD_LEN,
               sizeof(PV_ACCEPT_PLAIN)) != 0)
        return -2;
    if (memcmp(tag, PV_FRAME_ACCEPT + RADIO_PAIR_ACCEPT_AAD_LEN + sizeof(PV_ACCEPT_PLAIN),
               AEAD_TAG_BYTES) != 0)
        return -3;

    /* And back: sealing correctly says nothing about opening correctly. */
    memset(buf, 0, sizeof(buf));
    if (aead_open(PV_KEY_SESSION, PV_ACCEPT_NONCE, PV_ACCEPT_AAD,
                  sizeof(PV_ACCEPT_AAD),
                  PV_FRAME_ACCEPT + RADIO_PAIR_ACCEPT_AAD_LEN, sizeof(PV_ACCEPT_PLAIN),
                  buf, PV_FRAME_ACCEPT + RADIO_PAIR_ACCEPT_AAD_LEN + sizeof(PV_ACCEPT_PLAIN)) != 0)
        return -4;
    if (memcmp(buf, PV_ACCEPT_PLAIN, sizeof(PV_ACCEPT_PLAIN)) != 0)
        return -5;

    /* The uplink frame, which is what the hub actually opens all day. */
    memset(buf, 0, sizeof(buf));
    if (aead_open(PV_KEY_SESSION, PV_UPLINK_NONCE, PV_UPLINK_AAD,
                  sizeof(PV_UPLINK_AAD),
                  PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN, sizeof(PV_UPLINK_PLAIN),
                  buf, PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN + sizeof(PV_UPLINK_PLAIN)) != 0)
        return -6;
    if (memcmp(buf, PV_UPLINK_PLAIN, sizeof(PV_UPLINK_PLAIN)) != 0)
        return -7;

    /* A tampered tag must be refused, or the checks above never test the tag. */
    {
        uint8_t bad[AEAD_TAG_BYTES];

        memcpy(bad, PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN + sizeof(PV_UPLINK_PLAIN),
               sizeof(bad));
        bad[0] ^= 0x01u;
        if (aead_open(PV_KEY_SESSION, PV_UPLINK_NONCE, PV_UPLINK_AAD,
                      sizeof(PV_UPLINK_AAD),
                      PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN, sizeof(PV_UPLINK_PLAIN),
                      buf, bad) == 0)
            return -8;
    }

    /* And a tampered AAD, where the slot and superframe live.
     * radio_devices_docs/radio/crypto/wire-crypto.md */
    {
        uint8_t aad[RADIO_UPLINK_AAD_LEN];

        memcpy(aad, PV_UPLINK_AAD, sizeof(aad));
        aad[2] ^= 0x01u;                       /* the slot byte */
        if (aead_open(PV_KEY_SESSION, PV_UPLINK_NONCE, aad, sizeof(aad),
                      PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN, sizeof(PV_UPLINK_PLAIN),
                      buf, PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN + sizeof(PV_UPLINK_PLAIN)) == 0)
            return -9;
    }

    /* Sizes did not move v4 -> v5, so only the version sees a stale set.
     * radio_devices_docs/radio/crypto/wire-crypto.md */
    _Static_assert(LINK_VECTORS_VERSION == RADIO_LINK_VERSION,
                   "the link vectors are not the wire this build speaks");

    /* pair_v2's frames stay: they check GCM, not the wire this build speaks. */
    _Static_assert(sizeof(LV_UPLINK_PLAIN) == sizeof(radio_uplink_report_t),
                   "the uplink vector is not the report this build compiles");
    _Static_assert(sizeof(LV_DOWNLINK_PLAIN) == sizeof(radio_downlink_cmd_t),
                   "the downlink vector is not the command this build compiles");
    _Static_assert(sizeof(LV_FRAME_UPLINK) == sizeof(radio_uplink_t),
                   "the uplink vector is not the frame this build compiles");
    _Static_assert(sizeof(LV_FRAME_DOWNLINK) == sizeof(radio_downlink_t),
                   "the downlink vector is not the frame this build compiles");

    /* The pinned header is the AAD: unchecked, a frame could disagree with it. */
    if (memcmp(LV_FRAME_UPLINK, LV_UPLINK_AAD, sizeof(LV_UPLINK_AAD)) != 0)
        return -17;
    if (memcmp(LV_FRAME_DOWNLINK, LV_DOWNLINK_AAD, sizeof(LV_DOWNLINK_AAD)) != 0)
        return -18;
    if (LV_UPLINK_AAD[1] != RADIO_LINK_VERSION ||
        LV_DOWNLINK_AAD[1] != RADIO_LINK_VERSION)
        return -19;

    memset(buf, 0, sizeof(buf));
    if (aead_open(LV_KEY_SESSION, LV_UPLINK_NONCE, LV_UPLINK_AAD,
                  sizeof(LV_UPLINK_AAD),
                  LV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN, sizeof(LV_UPLINK_PLAIN),
                  buf, LV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN + sizeof(LV_UPLINK_PLAIN)) != 0)
        return -10;
    if (memcmp(buf, LV_UPLINK_PLAIN, sizeof(LV_UPLINK_PLAIN)) != 0)
        return -11;

    /* The downlink, which no vector covered before v4: the hub seals these. */
    memset(buf, 0, sizeof(buf));
    if (aead_open(LV_KEY_SESSION, LV_DOWNLINK_NONCE, LV_DOWNLINK_AAD,
                  sizeof(LV_DOWNLINK_AAD),
                  LV_FRAME_DOWNLINK + RADIO_DOWNLINK_AAD_LEN, sizeof(LV_DOWNLINK_PLAIN),
                  buf, LV_FRAME_DOWNLINK + RADIO_DOWNLINK_AAD_LEN +
                       sizeof(LV_DOWNLINK_PLAIN)) != 0)
        return -12;
    if (memcmp(buf, LV_DOWNLINK_PLAIN, sizeof(LV_DOWNLINK_PLAIN)) != 0)
        return -13;

    /* Seal must reproduce the published bytes; a round trip cannot show that. */
    {
        uint8_t ct[sizeof(LV_DOWNLINK_PLAIN)], tag[AEAD_TAG_BYTES];

        if (aead_seal(LV_KEY_SESSION, LV_DOWNLINK_NONCE, LV_DOWNLINK_AAD,
                      sizeof(LV_DOWNLINK_AAD), LV_DOWNLINK_PLAIN,
                      sizeof(LV_DOWNLINK_PLAIN), ct, tag) != 0)
            return -14;
        if (memcmp(ct, LV_FRAME_DOWNLINK + RADIO_DOWNLINK_AAD_LEN, sizeof(ct)) != 0)
            return -15;
        if (memcmp(tag, LV_FRAME_DOWNLINK + RADIO_DOWNLINK_AAD_LEN + sizeof(ct),
                   sizeof(tag)) != 0)
            return -16;
    }
    return 0;
}
