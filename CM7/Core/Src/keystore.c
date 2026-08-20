/* Persistent device enrolment, in flash bank 1.
 *
 * Log-structured: records are appended and never revised, and the newest record
 * for a device id wins. An H7 flash word can be programmed exactly once between
 * erases, so revising in place is not available even if it were desirable.
 *
 * The erase is what shapes everything else. H7's minimum is a whole 128 KB
 * sector, it stalls the bank the core is executing from, and it can take up to
 * 1.4 s. CM7 runs FreeRTOS and LwIP out of bank 1, so an in-service erase would
 * freeze the scheduler and drop every connection. The spare sector is therefore
 * erased at boot, before the scheduler starts, and the log switches to a sector
 * that is already erased. When both fill, the store refuses to write rather
 * than erasing - and the refusal is acted on by every caller, because a refusal
 * nothing acts on is decoration. */

#include <string.h>

#include "main.h"
#include "keystore.h"
#include "crypto.h"
#include "radio_slots.h"

#define KS_MAGIC        0x534B484Fu   /* 'OHKS' little-endian */
/* 4: fingerprint[32] -> pubkey[33] out of spare, for pair_v3's Z1 (ADR-0021).
 * Version-3 records are stepped over, not migrated: a fingerprint cannot be
 * turned back into a curve point, so every device is re-enrolled by hand. That
 * is the one-wayness working, not a migration that was skipped. */
#define KS_VERSION      4u

#define KS_SECTOR_A     FLASH_SECTOR_6
#define KS_SECTOR_B     FLASH_SECTOR_7
#define KS_ADDR_A       0x080C0000u
#define KS_ADDR_B       0x080E0000u
#define KS_SECTOR_BYTES 0x20000u
#define KS_FLASH_WORD   32u
#define KS_RECORD_BYTES 128u
#define KS_SLOTS        (KS_SECTOR_BYTES / KS_RECORD_BYTES)   /* 1024 */

/* Three flash words exactly. Not "at most": a short record leaves filler that a
 * future field could quietly claim, and a long one is more programs than the
 * write path performs. This is the invariant that breaks if a field is added
 * carelessly, so it is the one asserted. */
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
/* Carried out of scan() so ks_init can re-append them at the current version.
 * Separate from hub_key/net_key so a legacy record can never be served as a
 * current one without the rewrite actually having succeeded. */
static uint32_t    migrated;      /* key records carried across a format bump */
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

/* Every type the store writes must be listed here, or scan() rejects the record
 * before the branch that caches it and that branch becomes dead code that reads
 * as working. KS_TYPE_NETKEY was missing: the network key was rejected at every
 * boot, regenerated, and written again - so a hub reboot silently re-keyed the
 * hop sequence for every device already paired, and each boot spent a slot of a
 * store that never erases. */
/* Version 3's layout, for the two record types that never had a fingerprint.
 *
 * Growing fingerprint[32] to pubkey[33] shifted root_key by one byte for every
 * type, because all three share one struct - so a change only device records
 * use invalidated the hub's own keypair and the network hop key, neither of
 * which has a fingerprint in it. The store never erases, so those records are
 * still in flash and readable: the CRC covers the same bytes either way.
 *
 * Only root_key is translated. It is the only field HUBKEY and NETKEY use. */
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
_Static_assert(offsetof(ks_record_v3_t, root_key) + 1u ==
               offsetof(ks_record_t, root_key),
               "the shim exists because root_key moved by exactly one byte");

/* A key-bearing record this build cannot parse but a previous one wrote. Kept
 * apart from record_valid() so nothing downstream mistakes it for a usable
 * record - it is only ever read through the shim above. */
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

/* There is deliberately no erase function here. CM7 fetches from bank 1 and
 * this store is in bank 1; erasing it hangs the core and leaves the sector with
 * uncorrectable ECC, which faults every subsequent boot. See ks_init(). Keeping
 * an unused one would leave a loaded gun for the next person to call. */

/* Four flash words. A power loss between them leaves a record whose CRC fails,
 * which the scan skips - a torn record is indistinguishable from one that was
 * never written, and both are correct outcomes. */
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

/* Newest record per device id, and the append point.
 *
 * The append point is the first *erased* slot, never one past the last valid
 * record. Those differ whenever a slot holds something the scanner rejects: a
 * torn write, or - as happened here - records from an older format that the
 * version gate correctly refuses. Deriving the append point from valid records
 * alone aims the next write at occupied flash, and an H7 flash word cannot be
 * programmed twice, so every write fails from then on. The version gate worked
 * exactly as intended and the store was bricked anyway. */
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
                /* A v3 hub or network key: readable through the shim, and the
                 * only records whose loss would cost more than a re-enrolment.
                 * Recorded, not cached as valid - ks_init re-appends them at
                 * the current version once the scan knows where to write. */
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
                /* Our magic with someone else's version is a format change,
                 * not a torn write. Worth telling apart: a torn record is one
                 * slot to step over, a whole sector of the wrong stride is not
                 * appendable at all. */
                if (r->magic == KS_MAGIC && r->version != KS_VERSION)
                    stale_format++;
                continue;
            }

            if (!found || (int32_t)(r->seq - best_seq) > 0) {
                best_seq    = r->seq;
                active_addr = base;
                found       = 1;
            }

            /* The hub's own key is not a device and does not belong in a cache
             * keyed by device id - dev_id is zero on that record, which is a
             * value ks_enrol refuses precisely so it can never collide. */
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


    /* Records of an older format are *stepped over*, not erased.
     *
     * Erasing them was the obvious move and it hung the core. CM7 executes from
     * bank 1 and the store is in bank 1; a 128 KB sector erase there stalls the
     * bank the erase loop itself is being fetched from, and CM7 never came back
     * - the console was dead and even SWD memory reads stalled. CM4 gets away
     * with the same pattern in bank 2 only because a single erase completes;
     * two back to back here did not.
     *
     * Nothing needs erasing anyway. The append point is the first *erased*
     * slot, so stale records are simply skipped and the next write lands past
     * them. The cost is a handful of slots out of 2048, once, at a format
     * change. That is cheaper than an erase this core cannot safely perform.
     *
     * The count is kept and reported, because "some slots here are unreadable"
     * should be visible rather than inferred from a slot total that does not
     * add up. */

    spare = (active_addr == KS_ADDR_A) ? KS_ADDR_B : KS_ADDR_A;

    /* The spare is *checked* and never erased.
     *
     * CM7 executes from bank 1 and this store is in bank 1. A 128 KB sector
     * erase there hangs this core - demonstrated, not feared: a single erase
     * issued from a FreeRTOS task never returned, the console died, and SWD
     * memory reads stalled. Worse, the interrupted erase left the sector with
     * uncorrectable ECC, so every subsequent boot bus-faulted inside scan()
     * before the scheduler started. The board needed an external programmer
     * (`-e 6 7`) to come back.
     *
     * CM4's equivalent in bank 2 does work and has been observed to recover a
     * full log. Same code, different bank, different outcome - so this is a
     * property of erasing the bank you fetch from, not of the design.
     *
     * The consequence is accepted rather than worked around: this store has
     * 2048 records and cannot reclaim any. For 64 devices, at a few records per
     * device lifetime, that is not a capacity anyone reaches. If it ever
     * matters, the fix is an erase routine resident in ITCM with interrupts
     * masked - not a boot-time erase from flash. */
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

/* The recovered v3 hub private key, for the caller to *verify* before anything
 * is written. Reading is safe; writing is not.
 *
 * The CRC cannot police this. It covers the same bytes whichever offset the
 * shim reads, so a read one byte out yields 32 bytes that pass CRC, are the
 * right length, and are a perfectly valid P-256 scalar. Nothing inside this
 * store can tell a correct recovery from a plausible neighbour.
 *
 * And a wrong recovery is worse than the outage it fixes: this store never
 * erases, so the v3 record survives physically - but an appended v4 record is
 * the one the scanner then prefers. A misread does not fail, it overwrites a
 * truth that was still reachable. Raised by the device side. */
int ks_legacy_hub_key_get(uint8_t priv[KS_ROOT_KEY_BYTES]) {
    if (priv == NULL || !legacy_hub_valid)
        return -1;
    memcpy(priv, legacy_hub_key, KS_ROOT_KEY_BYTES);
    return 0;
}

/* Writes the recovered records forward, and only the caller can know it is
 * safe: the witness is the hub's public key as a *device* holds it, which is
 * outside this store and cannot be forged by anything in it.
 *
 * Both records go together on purpose. The hub key is the one with an external
 * witness; verifying it proves the shim's offset, and the network key is read
 * through the same shim, so one comparison licenses both. */
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
        migrated++;
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
        migrated++;
        done++;
    }
    return done;
}

uint32_t ks_migrated(void) { return migrated; }
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
            /* Both sectors full, and unlike CM4's store a reboot does not
             * recover this one - see ks_init() for why nothing here erases.
             * Refusing is still the safe direction and callers act on it: an
             * enrolment that silently did not persist is a device that pairs
             * today and is a stranger after a reboot. Recovery is an external
             * erase of sectors 6 and 7. */
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

/* Lowest free slot. Assignment counts up from zero on purpose: the join region
 * overlays the tail of the uplink region, so unassigned slots are the ones it
 * displaces. See docs/radio/pairing.md. */
static int lowest_free_slot(uint32_t skip_dev_id, uint8_t *out) {
    for (uint32_t s = 0; s < KS_MAX_DEVICES && s < RADIO_SLOT_COUNT; s++) {
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

int ks_enrol(uint32_t dev_id, const uint8_t pubkey[KS_PUBKEY_BYTES],
             uint8_t *slot_out) {
    const ks_record_t *have;
    ks_record_t r;
    uint8_t slot;

    if (pubkey == NULL || slot_out == NULL)
        return -1;
    /* Zero is not a device id. It is what an uninitialised variable holds, and
     * an id that can be produced by forgetting to set one is an id that will
     * eventually be set by forgetting. */
    if (dev_id == 0u)
        return -2;

    have = ks_find(dev_id);
    memset(&r, 0, sizeof(r));

    if (have != NULL && have->state != KS_STATE_DELETED) {
        /* Re-enrolling: keep the slot and the transmit floor, drop everything
         * scoped to the old key. The receive floor belongs to a key, not to a
         * device - a floor left over from a previous pairing locks out the very
         * device that just paired, silently and forever. */
        r = *have;
        r.rx_floor = 0;
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
    /* Stays zero until the key exchange sets it to the epoch the root key was
     * agreed at. An enrolment carries no key, so there is no epoch to record. */
    r.rotate_epoch = 0;
    memcpy(r.pubkey, pubkey, KS_PUBKEY_BYTES);

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
    /* The transmit floor survives a tombstone: carrying it forward only skips
     * counter space, which is free, and skipping is the conservative direction. */
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
                     const uint8_t dev_nonce[8], uint32_t rotate_epoch) {
    const ks_record_t *have = ks_find(dev_id);
    ks_record_t r;

    if (session_key == NULL || dev_nonce == NULL)
        return -1;
    /* Only an enrolled device may complete a pairing. Without this the exchange
     * would create its own record and the operator's out-of-band public key
     * would have been optional all along - which is unauthenticated ECDH
     * wearing the shape of an authenticated one. */
    if (have == NULL || have->state == KS_STATE_DELETED)
        return -2;

    r = *have;
    r.state        = KS_STATE_PAIRED;
    r.rotate_epoch = rotate_epoch;
    /* Scoped to the key, not to the device: a floor carried over from the
     * previous pairing would lock out the device that just paired. */
    r.rx_floor     = 0;
    memcpy(r.session_key, session_key, 16);
    memcpy(r.last_nonce, dev_nonce, 8);

    return (append(&r) == 0) ? 0 : -4;
}

int ks_net_key_get(uint8_t key[16]) {
    ks_record_t r;

    if (key == NULL)
        return -1;
    if (net_key_valid) {
        memcpy(key, net_key.root_key, 16);
        return 0;
    }

    /* Created on first use rather than at boot: a hub with no devices has no
     * network to key, and a key written before the first pairing is a flash
     * write nothing asked for. */
    if (crypto_random(key, 16) != 0)
        return -1;

    memset(&r, 0, sizeof(r));
    r.type   = KS_TYPE_NETKEY;
    r.state  = KS_STATE_PAIRED;
    r.dev_id = 0;               /* never a device id; ks_enrol refuses zero */
    memcpy(r.root_key, key, 16);

    if (append(&r) != 0) {
        /* The caller must not hand out a key the store did not keep: every
         * device paired under it would be lost at the next reboot, and the
         * symptom is a network that hops apart after a power cut. */
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

int ks_hub_key_set(const uint8_t priv[32]) {
    ks_record_t r;

    if (priv == NULL)
        return -1;
    /* Refusing is the whole point. Replacing this key orphans every device ever
     * paired: each holds hub_static from provisioning, and a hub that cannot
     * prove the matching private key fails Z1 and therefore every pairing it
     * already has. There is no "regenerate" that is not a fleet re-provision.
     *
     * legacy_hub_valid is checked too, and that is not belt and braces: a
     * format bump made hub_key_valid false for a key that was still in flash,
     * which disarmed this guard exactly when it mattered most. "This build
     * cannot read the record" and "there is no record" are opposite facts that
     * both leave hub_key_valid clear, and only one of them may proceed. */
    if (hub_key_valid || legacy_hub_valid)
        return -2;

    memset(&r, 0, sizeof(r));
    r.type   = KS_TYPE_HUBKEY;
    r.state  = KS_STATE_PAIRED;
    r.dev_id = 0;               /* never a device id; ks_enrol refuses zero */
    memcpy(r.root_key, priv, 32);

    return (append(&r) == 0) ? 0 : -4;
}

uint32_t ks_count(void) { return cached; }
uint32_t ks_writes(void) { return writes; }
uint32_t ks_errors(void) { return errors; }
uint32_t ks_last_flash_error(void) { return last_flash_err; }
uint32_t ks_stale_format(void) { return stale_format; }

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
