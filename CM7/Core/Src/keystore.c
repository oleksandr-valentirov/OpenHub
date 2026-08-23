/**
 * @file keystore.c
 * @brief Persistent device enrolment in flash bank 1: log-structured, newest record wins.
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */

#include <string.h>

#include "main.h"
#include "keystore.h"
#include "crypto.h"
#include "radio_slots.h"

#define KS_MAGIC        0x534B484Fu   /* 'OHKS' little-endian */
/* 5: pubkey is 32-byte X25519 and is empty at enrol. ADR-0024, ADR-0025
 * radio_devices_docs/open_hub/arch/keystore.md */
#define KS_VERSION      5u

#define KS_SECTOR_A     FLASH_SECTOR_6
#define KS_SECTOR_B     FLASH_SECTOR_7
#define KS_ADDR_A       0x080C0000u
#define KS_ADDR_B       0x080E0000u
#define KS_SECTOR_BYTES 0x20000u
#define KS_FLASH_WORD   32u
#define KS_RECORD_BYTES 128u
#define KS_SLOTS        (KS_SECTOR_BYTES / KS_RECORD_BYTES)   /* 1024 */

/* Three flash words exactly, not "at most".
 * radio_devices_docs/open_hub/arch/keystore.md */
_Static_assert(sizeof(ks_record_t) == KS_RECORD_BYTES,
               "ks_record_t must fill exactly four H7 flash words");
_Static_assert(KS_RECORD_BYTES % KS_FLASH_WORD == 0,
               "records must start on a flash-word boundary");

static ks_record_t cache[KS_MAX_DEVICES];
static ks_record_t hub_key;        /* the KS_TYPE_HUBKEY record, if one exists */
static uint8_t     hub_key_valid;
static ks_record_t net_key;        /* the KS_TYPE_NETKEY record, if one exists */
static uint8_t     net_key_valid;
static uint32_t    cached;
static uint32_t    active_addr;
static uint32_t    next_slot;
static uint32_t    last_seq;
static uint32_t    writes;
static uint32_t    errors;
static uint8_t     spare_erased;
static uint8_t     exhausted;
static uint8_t     ready;
/* Carried out of scan() separately, so a legacy record is never served as current.
 * radio_devices_docs/open_hub/arch/keystore.md */
static uint8_t     legacy_hub_key[KS_ROOT_KEY_BYTES];
static uint8_t     legacy_hub_valid;
static uint32_t    legacy_hub_seq;
static uint8_t     legacy_net_key[16];
static uint8_t     legacy_net_valid;
static uint32_t    legacy_net_seq;
static uint32_t    stale_format;   /* records whose magic is ours and version is not */
static uint32_t    last_flash_err;  /* HAL_FLASH_GetError() from the last failure */

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;

    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

static uint32_t record_crc(const ks_record_t *r) {
    return crc32(r, offsetof(ks_record_t, crc));
}

/* Every type the store writes must be listed here, or its cache branch is dead.
 * radio_devices_docs/open_hub/arch/keystore.md */

/* Version 3's layout; only root_key is translated.
 * radio_devices_docs/open_hub/arch/keystore.md */
typedef struct ks_record_v3 {
    uint32_t magic;
    uint8_t  version, type, state, slot;
    uint32_t seq, dev_id, key_gen, rotate_epoch, rx_floor, tx_floor;
    uint8_t  fingerprint[32];
    uint8_t  root_key[KS_ROOT_KEY_BYTES];
    uint8_t  session_key[16];
    uint8_t  last_nonce[8];
    uint8_t  spare[4];
    uint32_t crc;
} ks_record_v3_t;
_Static_assert(sizeof(ks_record_v3_t) == KS_RECORD_BYTES,
               "the v3 shim must describe the record that is actually in flash");
/* The v4 record's 33-byte pubkey moved root_key by one; ADR-0025's 32 moved it back.
 * radio_devices_docs/open_hub/arch/keystore.md */
_Static_assert(offsetof(ks_record_v3_t, root_key) ==
               offsetof(ks_record_t, root_key),
               "the v3 shim reads the same offsets again, and only the meaning differs");

/* A key record this build cannot parse, read only through the shim above.
 * radio_devices_docs/open_hub/arch/keystore.md */
static int legacy_key_record(const ks_record_t *r) {
    return r->magic == KS_MAGIC && r->version == 3u &&
           (r->type == KS_TYPE_HUBKEY || r->type == KS_TYPE_NETKEY) &&
           r->crc == record_crc(r);
}

static int append(const ks_record_t *src);

static int record_valid(const ks_record_t *r) {
    return r->magic == KS_MAGIC && r->version == KS_VERSION &&
           (r->type == KS_TYPE_DEVICE || r->type == KS_TYPE_HUBKEY ||
            r->type == KS_TYPE_NETKEY) &&
           r->crc == record_crc(r);
}

static int slot_erased(const void *p) {
    const uint32_t *w = p;

    for (unsigned i = 0; i < KS_RECORD_BYTES / 4u; i++)
        if (w[i] != 0xFFFFFFFFu)
            return 0;
    return 1;
}

/* There is deliberately no erase function here.
 * radio_devices_docs/open_hub/arch/keystore.md */

/* Four flash words; a power loss between them leaves a record the scan skips. */
static uint8_t write_record(uint32_t addr, const ks_record_t *r) {
    const uint8_t *src = (const uint8_t *)r;
    HAL_StatusTypeDef st = HAL_OK;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return 1;
    for (unsigned i = 0; i < KS_RECORD_BYTES / KS_FLASH_WORD; i++) {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                               addr + i * KS_FLASH_WORD,
                               (uint64_t)(uint32_t)(src + i * KS_FLASH_WORD));
        if (st != HAL_OK)
            break;
    }
    if (HAL_FLASH_Lock() != HAL_OK)
        return 1;
    return (st == HAL_OK) ? 0u : 1u;
}

static ks_record_t *cache_find(uint32_t dev_id) {
    for (uint32_t i = 0; i < cached; i++)
        if (cache[i].dev_id == dev_id)
            return &cache[i];
    return NULL;
}

/* Newest record per device id, and the append point: the first erased slot.
 * radio_devices_docs/open_hub/arch/keystore.md */
static void scan(void) {
    uint32_t best_seq = 0;
    uint32_t first_erased[2];
    int found = 0;

    cached        = 0;
    hub_key_valid = 0;
    net_key_valid = 0;
    active_addr = KS_ADDR_A;
    next_slot   = 0;
    last_seq    = 0;

    for (int s = 0; s < 2; s++) {
        uint32_t base = s ? KS_ADDR_B : KS_ADDR_A;

        first_erased[s] = KS_SLOTS;
        for (uint32_t i = 0; i < KS_SLOTS; i++) {
            const ks_record_t *r =
                (const ks_record_t *)(base + i * KS_RECORD_BYTES);
            ks_record_t *have;

            if (slot_erased(r)) {
                first_erased[s] = i;
                break;              /* the log is written in order */
            }
            if (!record_valid(r)) {
                /* Recorded, never cached as valid; ks_init re-appends them.
                 * radio_devices_docs/open_hub/arch/keystore.md */
                if (legacy_key_record(r)) {
                    const ks_record_v3_t *v3 = (const ks_record_v3_t *)r;

                    if (r->type == KS_TYPE_HUBKEY &&
                        (!legacy_hub_valid ||
                         (int32_t)(r->seq - legacy_hub_seq) > 0)) {
                        memcpy(legacy_hub_key, v3->root_key, KS_ROOT_KEY_BYTES);
                        legacy_hub_seq = r->seq;
                        legacy_hub_valid = 1;
                    }
                    if (r->type == KS_TYPE_NETKEY &&
                        (!legacy_net_valid ||
                         (int32_t)(r->seq - legacy_net_seq) > 0)) {
                        memcpy(legacy_net_key, v3->root_key, 16);
                        legacy_net_seq = r->seq;
                        legacy_net_valid = 1;
                    }
                }
                /* Our magic at someone else's version is a format change.
                 * radio_devices_docs/open_hub/arch/keystore.md */
                if (r->magic == KS_MAGIC && r->version != KS_VERSION)
                    stale_format++;
                continue;
            }

            if (!found || (int32_t)(r->seq - best_seq) > 0) {
                best_seq    = r->seq;
                active_addr = base;
                found       = 1;
            }

            /* The hub's own key is not a device; its dev_id is the refused zero.
             * radio_devices_docs/open_hub/arch/keystore.md */
            if (r->type == KS_TYPE_HUBKEY) {
                if (!hub_key_valid || (int32_t)(r->seq - hub_key.seq) > 0) {
                    hub_key = *r;
                    hub_key_valid = 1;
                }
                continue;
            }
            if (r->type == KS_TYPE_NETKEY) {
                if (!net_key_valid || (int32_t)(r->seq - net_key.seq) > 0) {
                    net_key = *r;
                    net_key_valid = 1;
                }
                continue;
            }

            have = cache_find(r->dev_id);
            if (have != NULL) {
                if ((int32_t)(r->seq - have->seq) > 0)
                    *have = *r;
            } else if (cached < KS_MAX_DEVICES) {
                cache[cached++] = *r;
            } else {
                errors++;           /* more device ids on flash than fit */
            }
        }
    }

    last_seq  = best_seq;
    /* With nothing valid anywhere, prefer whichever sector has room. */
    if (!found && first_erased[0] >= KS_SLOTS && first_erased[1] < KS_SLOTS)
        active_addr = KS_ADDR_B;
    next_slot = first_erased[(active_addr == KS_ADDR_B) ? 1 : 0];
}

uint8_t ks_init(void) {
    uint32_t spare;

    stale_format = 0;
    scan();


    /* Older formats are stepped over, never erased, and the count is reported.
     * radio_devices_docs/open_hub/arch/keystore.md */

    spare = (active_addr == KS_ADDR_A) ? KS_ADDR_B : KS_ADDR_A;

    /* The spare is checked and never erased: an erase in bank 1 hangs this core.
     * radio_devices_docs/open_hub/arch/keystore.md */
    spare_erased = 1;
    for (uint32_t i = 0; i < KS_SLOTS; i++) {
        if (!slot_erased((const void *)(spare + i * KS_RECORD_BYTES))) {
            spare_erased = 0;
            break;
        }
    }

    exhausted = 0;
    ready = 1;

    /* Nothing is migrated here, deliberately. See ks_legacy_commit(). */
    return 0;
}

/* The recovered v3 hub private key, for the caller to verify before writing.
 * radio_devices_docs/open_hub/arch/keystore.md */
int ks_legacy_hub_key_get(uint8_t priv[KS_ROOT_KEY_BYTES]) {
    if (priv == NULL || !legacy_hub_valid)
        return -1;
    memcpy(priv, legacy_hub_key, KS_ROOT_KEY_BYTES);
    return 0;
}

/* Writes the recovered records forward, both together, once a witness agrees.
 * radio_devices_docs/open_hub/arch/keystore.md */
int ks_legacy_commit(void) {
    ks_record_t r;
    int done = 0;

    if (legacy_hub_valid && !hub_key_valid) {
        memset(&r, 0, sizeof(r));
        r.type   = KS_TYPE_HUBKEY;
        r.state  = KS_STATE_PAIRED;
        r.dev_id = 0;
        memcpy(r.root_key, legacy_hub_key, KS_ROOT_KEY_BYTES);
        if (append(&r) != 0)
            return -1;
        done++;
    }
    if (legacy_net_valid && !net_key_valid) {
        memset(&r, 0, sizeof(r));
        r.type   = KS_TYPE_NETKEY;
        r.state  = KS_STATE_PAIRED;
        r.dev_id = 0;
        memcpy(r.root_key, legacy_net_key, 16);
        if (append(&r) != 0)
            return -1;
        done++;
    }
    return done;
}

int ks_legacy_pending(void) {
    return (legacy_hub_valid && !hub_key_valid) ||
           (legacy_net_valid && !net_key_valid);
}

static int append(const ks_record_t *src) {
    ks_record_t r __attribute__((aligned(32)));
    uint32_t addr;

    if (!ready || exhausted)
        return -1;

    if (next_slot >= KS_SLOTS) {
        if (!spare_erased) {
            /* Both sectors full; recovery is an external erase of 6 and 7.
             * radio_devices_docs/open_hub/arch/keystore.md */
            errors++;
            exhausted = 1;
            return -1;
        }
        active_addr  = (active_addr == KS_ADDR_A) ? KS_ADDR_B : KS_ADDR_A;
        next_slot    = 0;
        spare_erased = 0;               /* the stale sector is erased at next boot */
    }

    r = *src;
    r.magic   = KS_MAGIC;
    r.version = KS_VERSION;
    r.seq     = ++last_seq;
    r.crc     = record_crc(&r);

    addr = active_addr + next_slot * KS_RECORD_BYTES;
    if (write_record(addr, &r) != 0) {
        errors++;
        exhausted = 1;   /* a flash that refuses one write will refuse the next */
        last_seq--;
        return -1;
    }
    next_slot++;
    writes++;

    if (r.type == KS_TYPE_HUBKEY) {
        hub_key = r;
        hub_key_valid = 1;
        return 0;
    }
    if (r.type == KS_TYPE_NETKEY) {
        net_key = r;
        net_key_valid = 1;
        return 0;
    }

    {
        ks_record_t *have = cache_find(r.dev_id);

        if (have != NULL) {
            *have = r;
        } else if (cached < KS_MAX_DEVICES) {
            cache[cached++] = r;
        } else {
            errors++;
            return -1;
        }
    }
    return 0;
}

/* Lowest free slot, counting up from zero. See radio_devices_docs/radio/pairing.md. */
static int lowest_free_slot(uint32_t skip_dev_id, uint8_t *out) {
    /* A slot is no longer a device index; RADIO_DEVICE_MAX is the cap.
     * radio_devices_docs/radio/tdma.md */
    for (uint32_t s = 0; s < KS_MAX_DEVICES && s < RADIO_DEVICE_MAX; s++) {
        uint32_t i;

        for (i = 0; i < cached; i++) {
            if (cache[i].state == KS_STATE_DELETED)
                continue;
            if (cache[i].dev_id == skip_dev_id)
                continue;
            if (cache[i].slot == (uint8_t)s)
                break;
        }
        if (i == cached) {
            *out = (uint8_t)s;
            return 0;
        }
    }
    return -1;
}

int ks_enrol(uint32_t dev_id, uint8_t *slot_out) {
    const ks_record_t *have;
    ks_record_t r;
    uint8_t slot;

    if (slot_out == NULL)
        return -1;
    /* Zero is not a device id: it is what an uninitialised variable holds. */
    if (dev_id == 0u)
        return -2;

    have = ks_find(dev_id);
    memset(&r, 0, sizeof(r));

    if (have != NULL && have->state != KS_STATE_DELETED) {
        /* Re-enrolling keeps the slot and the transmit floor, and drops the rest.
         * radio_devices_docs/open_hub/arch/keystore.md */
        r = *have;
        r.rx_floor = 0;
        memset(r.pubkey, 0, sizeof(r.pubkey));
        r.key_gen  = have->key_gen + 1u;
        memset(r.root_key, 0, sizeof(r.root_key));
        slot = have->slot;
    } else {
        if (lowest_free_slot(dev_id, &slot) != 0)
            return -3;
        r.dev_id   = dev_id;
        r.slot     = slot;
        r.key_gen  = (have != NULL) ? have->key_gen + 1u : 1u;
        r.tx_floor = (have != NULL) ? have->tx_floor : 0u;
    }

    r.type  = KS_TYPE_DEVICE;
    r.state = KS_STATE_ENROLLED;
    /* Zero until the key exchange sets it: an enrolment carries no key. */
    r.rotate_epoch = 0;
    /* The device's key arrives in PAIR_REQ; enrolment is an id and nothing else. */
    memset(r.pubkey, 0, KS_PUBKEY_BYTES);

    if (append(&r) != 0)
        return -4;

    *slot_out = slot;
    return 0;
}

int ks_forget(uint32_t dev_id) {
    const ks_record_t *have = ks_find(dev_id);
    ks_record_t r;

    if (have == NULL || have->state == KS_STATE_DELETED)
        return -1;

    r = *have;
    r.type  = KS_TYPE_DEVICE;
    r.state = KS_STATE_DELETED;
    /* The transmit floor survives a tombstone: skipping counter space is free. */
    r.rx_floor = 0;
    r.key_gen  = have->key_gen + 1u;
    memset(r.root_key, 0, sizeof(r.root_key));
    memset(r.pubkey, 0, sizeof(r.pubkey));

    return append(&r);
}

int ks_write_torn(void) {
    ks_record_t r __attribute__((aligned(32)));
    const uint8_t *src = (const uint8_t *)&r;
    uint32_t addr;
    HAL_StatusTypeDef st = HAL_OK;

    if (!ready || exhausted || next_slot >= KS_SLOTS)
        return -1;

    memset(&r, 0, sizeof(r));
    r.magic   = KS_MAGIC;
    r.version = KS_VERSION;
    r.type    = KS_TYPE_DEVICE;
    r.state   = KS_STATE_ENROLLED;
    r.dev_id  = 0xDEADBEEFu;
    r.slot    = 0;
    r.seq     = last_seq + 1000u;   /* would win every comparison if accepted */
    r.crc     = record_crc(&r);

    addr = active_addr + next_slot * KS_RECORD_BYTES;
    if (HAL_FLASH_Unlock() != HAL_OK)
        return -1;
    /* Every word but the last. */
    for (unsigned i = 0; i < KS_RECORD_BYTES / KS_FLASH_WORD - 1u; i++) {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                               addr + i * KS_FLASH_WORD,
                               (uint64_t)(uint32_t)(src + i * KS_FLASH_WORD));
        if (st != HAL_OK)
            break;
    }
    if (HAL_FLASH_Lock() != HAL_OK)
        return -1;
    if (st != HAL_OK)
        return -1;

    next_slot++;    /* the torn record occupies its slot; it is not reused */
    return 0;
}

int ks_pair_complete(uint32_t dev_id, const uint8_t session_key[16],
                     const uint8_t dev_nonce[8], uint32_t rotate_epoch,
                     const uint8_t pubkey[KS_PUBKEY_BYTES]) {
    const ks_record_t *have = ks_find(dev_id);
    ks_record_t r;

    if (session_key == NULL || dev_nonce == NULL || pubkey == NULL)
        return -1;
    /* Only an enrolled device may complete a pairing.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (have == NULL || have->state == KS_STATE_DELETED)
        return -2;

    r = *have;
    r.state        = KS_STATE_PAIRED;
    r.rotate_epoch = rotate_epoch;
    /* Scoped to the key, never to the device.
     * radio_devices_docs/radio/crypto/key-lifecycle.md */
    r.rx_floor     = 0;
    memcpy(r.session_key, session_key, 16);
    memcpy(r.last_nonce, dev_nonce, 8);
    /* The one write of the device's key: a failed exchange leaves no trace.
     * radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md */
    memcpy(r.pubkey, pubkey, KS_PUBKEY_BYTES);

    return (append(&r) == 0) ? 0 : -4;
}

int ks_has_pubkey(const ks_record_t *rec) {
    if (rec == NULL)
        return 0;
    for (uint32_t i = 0; i < KS_PUBKEY_BYTES; i++)
        if (rec->pubkey[i] != 0u)
            return 1;
    return 0;
}

int ks_net_key_get(uint8_t key[16]) {
    ks_record_t r;

    if (key == NULL)
        return -1;
    if (net_key_valid) {
        memcpy(key, net_key.root_key, 16);
        return 0;
    }

    /* Created on first use: a hub with no devices has no network to key. */
    if (crypto_random(key, 16) != 0)
        return -1;

    memset(&r, 0, sizeof(r));
    r.type   = KS_TYPE_NETKEY;
    r.state  = KS_STATE_PAIRED;
    r.dev_id = 0;               /* never a device id; ks_enrol refuses zero */
    memcpy(r.root_key, key, 16);

    if (append(&r) != 0) {
        /* Never hand out a key the store did not keep.
         * radio_devices_docs/open_hub/arch/keystore.md */
        memset(key, 0, 16);
        return -4;
    }
    return 0;
}

int ks_hub_key_get(uint8_t priv[32]) {
    if (priv == NULL || !hub_key_valid)
        return -1;
    memcpy(priv, hub_key.root_key, 32);
    return 0;
}

static int hub_key_write(const uint8_t priv[32], int replacing_legacy) {
    ks_record_t r;

    if (priv == NULL)
        return -1;
    /* legacy_hub_valid too: absent and unreadable are opposite facts.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (hub_key_valid || (legacy_hub_valid && !replacing_legacy))
        return -2;

    memset(&r, 0, sizeof(r));
    r.type   = KS_TYPE_HUBKEY;
    r.state  = KS_STATE_PAIRED;
    r.dev_id = 0;               /* never a device id; ks_enrol refuses zero */
    memcpy(r.root_key, priv, 32);

    if (append(&r) != 0)
        return -4;
    /* The unreadable record is stepped over from here on, not consulted. */
    if (replacing_legacy)
        legacy_hub_valid = 0;
    return 0;
}

int ks_hub_key_set(const uint8_t priv[32]) {
    return hub_key_write(priv, 0);
}

int ks_hub_key_set_replacing_legacy(const uint8_t priv[32]) {
    return hub_key_write(priv, 1);
}

uint32_t ks_count(void) { return cached; }
uint32_t ks_writes(void) { return writes; }
uint32_t ks_errors(void) { return errors; }
uint32_t ks_last_flash_error(void) { return last_flash_err; }
uint32_t ks_stale_format(void) { return stale_format; }

uint8_t ks_exhausted(void) {
    return exhausted;
}

uint32_t ks_slots_left(void) {
    uint32_t left = (next_slot < KS_SLOTS) ? KS_SLOTS - next_slot : 0u;

    return spare_erased ? left + KS_SLOTS : left;
}

const ks_record_t *ks_at(uint32_t index) {
    return (index < cached) ? &cache[index] : NULL;
}

const ks_record_t *ks_find(uint32_t dev_id) {
    return cache_find(dev_id);
}
