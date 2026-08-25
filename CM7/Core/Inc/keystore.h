#pragma once

#include <stdint.h>

#include "radio_layout.h"

/**
 * @file keystore.h
 * @brief Enrolled devices, their keys and their slots, in an append-only flash log.
 *
 * ADR-0021. The store never erases, because erasing bank 1 from CM7 does not
 * return. radio_devices_docs/open_hub/arch/keystore.md
 */

#define KS_FINGERPRINT_BYTES  32u   /* SHA-256 of the point; displayed, not stored */
#define KS_PUBKEY_BYTES       32u   /* X25519 u-coordinate - ADR-0025 */
#define KS_ROOT_KEY_BYTES     32u
#define KS_MAX_DEVICES        64u

/* The store must hold every device the grid can carry.
 * radio_devices_docs/radio/tdma.md */
_Static_assert(KS_MAX_DEVICES >= RADIO_DEVICE_MAX, "the store is smaller than the grid");

enum {
    KS_TYPE_DEVICE = 1,
    KS_TYPE_HUBKEY = 2,     /**< the hub's own long-term private key */
    /* The network hop key, the same 16 bytes for every device.
     * radio_devices_docs/radio/crypto/key-lifecycle.md */
    KS_TYPE_NETKEY = 3
};

enum {
    KS_STATE_FREE = 0,
    KS_STATE_ENROLLED,   /**< operator supplied a public key; not yet paired */
    KS_STATE_PAIRED,     /**< key exchange completed, root key present */
    KS_STATE_DELETED     /**< tombstone: the log is append-only */
};

/** @brief One record: 128 bytes, which is four H7 flash words exactly. */
typedef struct ks_record {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  state;
    uint8_t  slot;                              /**< uplink slot, assigned at enrol */
    uint32_t seq;                               /**< newest record for a dev_id wins */
    uint32_t dev_id;
    uint32_t key_gen;                           /**< generation rx_floor belongs to */
    uint32_t rotate_epoch;                      /**< superframe / SUPERFRAME_PER_DAY at pairing */
    uint32_t rx_floor;                          /**< scoped to key_gen, never to a device */
    uint32_t tx_floor;                          /**< carried forward across pairings */
    uint8_t  pubkey[KS_PUBKEY_BYTES];           /**< zero until PAIR_REQ brings it - ADR-0024 */
    uint8_t  root_key[KS_ROOT_KEY_BYTES];       /**< HKDF root; zero until pairing lands */
    uint8_t  session_key[16];                   /**< stored, not re-derived: Z is not kept */
    uint8_t  last_nonce[8];                     /**< the last accepted; a repeat is refused */
    uint8_t  spare[4];                          /**< named padding, so a field can be added */
    uint32_t crc;
} ks_record_t;

/**
 * @brief Scans the log and builds the in-RAM cache; run before the scheduler starts.
 * @retval 0  always: an empty store is a usable one
 *
 * Recovery may erase a 128 KB sector, stalling the bank FreeRTOS executes from.
 * radio_devices_docs/open_hub/arch/keystore.md
 */
uint8_t ks_init(void);

/**
 * @brief Enrols a device at the lowest free slot, persisting before any window opens.
 * @param dev_id    the device being enrolled
 * @param pubkey    its 32-byte X25519 u-coordinate, little-endian
 * @param slot_out  receives the slot assigned; no operator ever picks one
 * @retval  0  written to flash
 * @retval !=0 the store refused it; no window may be opened
 *
 * Re-enrolling drops the session key and bumps key_gen, which unpairs a working
 * device. radio_devices_docs/open_hub/arch/keystore.md
 */
int ks_enrol(uint32_t dev_id,
             uint8_t *slot_out);

/**
 * @brief Tombstones a device, freeing its slot; the log itself is append-only.
 * @param dev_id  the device to forget
 * @retval  0  it was there and a tombstone was written
 * @retval !=0 no such device, or the write failed
 */
int ks_forget(uint32_t dev_id);

/**
 * @brief Completes a pairing, moving the record to KS_STATE_PAIRED.
 * @param dev_id        the device that paired
 * @param session_key   stored rather than re-derived, because Z is not kept
 * @param dev_nonce     the last accepted nonce; a repeat is refused afterwards
 * @param rotate_epoch  the epoch the key belongs to
 * @param pubkey        the device's key as PAIR_REQ carried it
 * @retval  0  the record reached flash
 * @retval !=0 it did not, and the device must not be treated as paired
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */
int ks_pair_complete(uint32_t dev_id, const uint8_t session_key[16],
                     const uint8_t dev_nonce[8], uint32_t rotate_epoch,
                     const uint8_t pubkey[KS_PUBKEY_BYTES]);

/** @brief Nonzero once a device's key is known; an enrolment alone has none. */
int ks_has_pubkey(const ks_record_t *rec);

/**
 * @brief The network hop key, created on first use and shared by every device.
 * @param key  receives the 16 bytes
 * @retval  0  copied out
 * @retval !=0 it could not be created or read
 *
 * radio_devices_docs/radio/pairing.md
 */
int ks_net_key_get(uint8_t key[16]);

/**
 * @brief The hub's long-term private key.
 * @param priv  receives the scalar
 * @retval  0  one is stored and was copied out
 * @retval !=0 none is stored
 */
int ks_hub_key_get(uint8_t priv[32]);

/**
 * @brief Stores the hub's long-term private key, once.
 * @param priv  the scalar to keep
 * @retval  0  stored
 * @retval -2  one already exists and was not overwritten
 * @retval !=0 the write failed
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */
int ks_hub_key_set(const uint8_t priv[32]);

/** @brief Same, over a record of a retired format a curve change has orphaned. */
int ks_hub_key_set_replacing_legacy(const uint8_t priv[32]);

/**
 * @brief Records the cache holds, tombstones included.
 * @return count, which is why a listing's total is computed from what it printed
 */
uint32_t ks_count(void);

/**
 * @brief One record by cache index.
 * @param index  0..ks_count()-1
 * @return the record, or NULL when @p index is past the end
 */
const ks_record_t *ks_at(uint32_t index);

/**
 * @brief The newest record for a device, which is the one that wins.
 * @param dev_id  the device
 * @return its record, or NULL when nothing is enrolled for it
 */
const ks_record_t *ks_find(uint32_t dev_id);

/**
 * @brief Test scaffolding: writes a record short by its final flash word.
 * @retval  0  the torn record reached flash at a higher seq than the good one
 * @retval !=0 it did not, so the scanner was never tested
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */
int ks_write_torn(void);

/**
 * Why an append last refused. `errors` counted four unrelated conditions with
 * one number and three of them never touch flash, so "flash write failed" was
 * printed for a cache that was full and for a scan that overflowed at boot.
 * radio_devices_docs/open_hub/arch/keystore.md
 */
typedef enum {
    KS_FAIL_NONE = 0,   /**< nothing has failed since boot */
    KS_FAIL_NOT_READY,  /**< ks_init() has not run */
    KS_FAIL_LATCHED,    /**< an earlier write failed and set the latch */
    KS_FAIL_LOG_FULL,   /**< both sectors used; only an external erase reclaims */
    KS_FAIL_UNLOCK,     /**< HAL_FLASH_Unlock refused */
    KS_FAIL_PROGRAM,    /**< HAL_FLASH_Program refused; the HAL code is recorded */
    KS_FAIL_LOCK,       /**< the write landed and HAL_FLASH_Lock refused */
    KS_FAIL_CACHE_FULL, /**< flash took the record and the RAM cache would not */
    KS_FAIL_SCAN_OVER,  /**< boot found more device ids on flash than fit */
    KS_FAIL_RETIRED     /**< the configuration store owns these sectors now */
} ks_fail_t;

/**
 * @brief Stops this store reading or writing, because the ring owns its sectors.
 *
 * Called once the configuration store is live. Without it this log would find an
 * erased sector 6 at the next boot and append into the ring's spare.
 * radio_devices_docs/open_hub/arch/config-store.md
 */
void ks_retire(void);

/** @brief Whether this store has been retired. @retval 1 it has */
int ks_retired(void);

/** @brief Records written since boot. @return count */
uint32_t ks_writes(void);

/** @brief Writes that failed, counted apart so a full store is not silent. @return count */
uint32_t ks_errors(void);

/**
 * @brief The last HAL flash error, so a refusal names itself.
 * @return the raw HAL_FLASH_GetError() code, or 0 if no write reached flash
 *
 * **Zero is ambiguous on its own** and must be read beside ks_last_fail():
 * three of the failure reasons never call into the HAL at all.
 */
uint32_t ks_last_flash_error(void);

/** @brief Why the last append refused. @return the reason, KS_FAIL_NONE if none has */
ks_fail_t ks_last_fail(void);

/** @brief The reason as a word for a console line. @return a static string, never NULL */
const char *ks_fail_str(ks_fail_t f);

/**
 * @brief Errors that were a real flash refusal, not a cache or a scan.
 * @return count; the difference from ks_errors() is the part flash never saw
 */
uint32_t ks_flash_errors(void);

/**
 * @brief Whether the cache can take a device id it does not already hold.
 * @retval 1  full: a new id cannot be served, though flash would take the write
 * @retval 0  there is room
 *
 * One entry per **distinct id ever written**, not per live device, so a store
 * whose roster is one device can still be full.
 * radio_devices_docs/open_hub/arch/keystore.md
 */
uint8_t ks_cache_full(void);

/** @brief Ids the cache holds, live and deleted alike. @return count */
uint32_t ks_cached(void);

/**
 * @brief Slots holding this store's magic at a different version.
 * @return count; they are skipped rather than reclaimed, since the log never erases
 */
uint32_t ks_stale_format(void);

/**
 * @brief Reads a hub key from a record written before the format changed.
 * @param priv  receives the scalar
 * @retval  0  a legacy key was found and copied out
 * @retval !=0 none is pending
 *
 * Read, witness, then commit: the migration is not applied until an outside
 * reader has seen the value. radio_devices_docs/open_hub/arch/keystore.md
 */
int      ks_legacy_hub_key_get(uint8_t priv[KS_ROOT_KEY_BYTES]);

/**
 * @brief Whether a legacy record is waiting to be migrated.
 * @retval 1  one is pending
 * @retval 0  nothing to do
 */
int      ks_legacy_pending(void);

/**
 * @brief Rewrites the pending legacy record in the current format.
 * @retval  0  migrated
 * @retval !=0 the write failed and the legacy record still stands
 */
int      ks_legacy_commit(void);

/**
 * @brief Room left in the append-only log.
 * @return records still writable, which is the number the exhausted path acts on
 */
uint32_t ks_slots_left(void);

/**
 * @brief Whether the log has stopped accepting records for good.
 * @retval 1  full or refusing writes; recovery is an external erase
 * @retval 0  still writable
 *
 * Distinct from ks_slots_left() == 0: a flash that refused one write is
 * exhausted with slots still counted free.
 */
uint8_t ks_exhausted(void);

/* Below this a full roster cannot be re-enrolled and re-paired.
 * radio_devices_docs/open_hub/arch/keystore.md */
#define KS_SLOTS_LOW  (2u * KS_MAX_DEVICES)
