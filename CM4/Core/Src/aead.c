#include <string.h>

#include "main.h"
#include "aead.h"
#include "radio_protocol.h"
#include "pair_v2.h"

extern CRYP_HandleTypeDef hcryp;

#define CRYP_TIMEOUT_MS  50u

/* The largest plaintext either direction carries: PAIR_ACCEPT's 19-byte grant.
 * Rounded up to a whole 32-bit word because of the defect below. */
#define AEAD_MAX_LEN  32u

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
 * apply to them - it swaps the *data* path only. A plain memcpy from a byte
 * array therefore loads each word reversed on this little-endian core, which
 * is a perfectly good cipher under a key nobody else has. Caught by the vector
 * self-test rather than reasoned about: the seal produced clean-looking
 * ciphertext that simply was not the published bytes. */
static void words_be(uint32_t *w, const uint8_t *b, unsigned n_words) {
    for (unsigned i = 0; i < n_words; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
}

/* The accelerator wants a 16-byte initial counter block, not the 12-byte nonce:
 * the nonce followed by the GCM counter's starting value of 2. */
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
    /* 8-bit, not the 32-bit the .ioc configures. With a 32-bit datatype the
     * accelerator takes the buffer word-wise, so on this little-endian core
     * every group of four bytes is reversed - a perfectly good cipher, and not
     * the one the far side computes. The hop PRF was measured with the same
     * trap and the same answer. */
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

    /* Zeroed before the copy for the same reason as aead_open, even though only
     * decrypt is known to be affected. A buffer whose tail is defined on one
     * path and stale on the other is a difference nobody will remember. */
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

    /* THE trap. HAL_CRYP_Decrypt in GCM mode does not mask the unused bytes of
     * a partial final word, while encrypt does. Whatever is left in this buffer
     * past `len` is fed to the accelerator and lands in the tag, so every length
     * that is not a multiple of four fails with byte-perfect ciphertext - which
     * on air looks like a radio fault and not a software one. The 19-byte grant
     * and the 23-byte wire_v3 case both sit on this. */
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

    /* Constant time, and the plaintext is not handed back until it verifies:
     * releasing unauthenticated bytes and reporting the failure separately is
     * how a caller that ignores a return value gets to act on forged data. */
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

    /* And back, which is the direction the defect is in. Sealing correctly says
     * nothing about opening correctly - they are different HAL paths. */
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

    /* A tampered tag must be refused. Without this the three checks above pass
     * identically on an implementation that never compares the tag at all. */
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

    /* And a tampered AAD, which is the half a tag check over ciphertext alone
     * would miss - the slot and superframe live there, and they are what binds
     * a frame to the device and the moment it claims. */
    {
        uint8_t aad[RADIO_UPLINK_AAD_LEN];

        memcpy(aad, PV_UPLINK_AAD, sizeof(aad));
        aad[2] ^= 0x01u;                       /* the slot byte */
        if (aead_open(PV_KEY_SESSION, PV_UPLINK_NONCE, aad, sizeof(aad),
                      PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN, sizeof(PV_UPLINK_PLAIN),
                      buf, PV_FRAME_UPLINK + RADIO_UPLINK_AAD_LEN + sizeof(PV_UPLINK_PLAIN)) == 0)
            return -9;
    }
    return 0;
}
