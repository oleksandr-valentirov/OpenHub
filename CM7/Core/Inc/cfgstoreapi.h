/**
 * @file cfgstoreapi.h
 * @brief The configuration store above the flash: boot, the RAM image, appends.
 *
 * ADR-0027 §8. radio_devices_docs/open_hub/arch/config-store.md
 */
#ifndef CFGSTOREAPI_H
#define CFGSTOREAPI_H

#include <stdint.h>

#include "cfgflash.h"
#include "cfgjournal.h"
#include "cfgstore.h"

/**
 * @brief Scans both journal sectors, copies out the newest snapshot, replays its tail.
 * @retval  0  a store was found and the image is live
 * @retval -1  no valid snapshot anywhere, which is an empty store and not a fault
 *
 * Reads only. Nothing older than the winning snapshot is read, ever.
 */
int cfg_init(void);

/**
 * @brief What the boot erase did, if the last wrap left a ring to reclaim.
 * @param ms_out  receives how long it took, or NULL
 * @param ran     receives 1 when an erase actually happened, or NULL
 * @return CFGF_OK when nothing was owed or the erase succeeded, else why it did not
 *
 * A boot erase that happened and said nothing is a decorative one.
 */
cfgflash_err_t cfg_boot_erase(uint32_t *ms_out, uint8_t *ran);

/**
 * @brief Whether an erase would have been permitted where the boot work runs.
 * @retval 1  HSEM_ID_0 was free, so the boot erase path is reachable
 * @retval 0  CM4 still held it, and every boot erase would refuse
 *
 * Read once at boot. Without it the boot erase is a path nothing can show is
 * reachable until the day it is needed.
 */
int cfg_boot_erase_was_legal(void);

/**
 * @brief What opening the ring did, if this boot was the one that opened it.
 * @param carried   receives devices carried out of the old log, or NULL
 * @param erase_ms  receives how long the ring's erase took, or NULL
 * @param erase_rc  receives that erase's result, or NULL
 * @return CFGF_OK when a ring was opened, else why it was not
 */
cfgflash_err_t cfg_open_result(uint32_t *carried, uint32_t *erase_ms,
                               cfgflash_err_t *erase_rc);

/** @brief Whether this boot wrote the first checkpoint. @retval 1 it did */
int cfg_ring_was_opened(void);

/** @brief The reconstructed image. @return the RAM copy, never NULL */
const cfg_snapshot_t *cfg_image(void);

/** @brief Where the scan left the ring. @return the scan record, never NULL */
const cfg_scan_t *cfg_where(void);

/** @brief Deltas appended since the last checkpoint. @return the count */
uint16_t cfg_since_snapshot(void);

/**
 * @brief Reads the newest valid identity record out of the identity sector.
 * @param out  receives it
 * @retval  0  one is stored and was copied out
 * @retval -1  the sector holds none, which is not a fault
 */
int cfg_identity_read(cfg_identity_t *out);

/** Which store answered the last key read. */
typedef enum {
    CFG_SRC_NONE = 0,
    CFG_SRC_STORE,    /**< the identity sector */
    CFG_SRC_OLD_LOG   /**< the append-only keystore, which is being retired */
} cfg_src_t;

/** @brief Which store served the last hub_key_get(). @return the source */
cfg_src_t hub_key_source(void);

/**
 * @brief The hub's scalar: the new store if it has one, else the old log.
 * @param priv  receives it
 * @retval  0  copied out
 * @retval -1  neither store holds one
 *
 * One seam, so the fallback order is stated once rather than at every call site.
 */
int hub_key_get(uint8_t priv[CFG_ROOT_KEY_BYTES]);

/**
 * @brief The network hop key, the same way, and it never creates one.
 * @param key  receives it
 * @retval  0  copied out
 * @retval -1  neither store holds one
 */
int hub_net_key_get(uint8_t key[CFG_SESSION_BYTES]);

/**
 * @brief The hub's long-term scalar, from the identity sector.
 * @param priv  receives it
 * @retval  0  an identity record exists and was copied out
 * @retval -1  none is stored; the caller must fall back to the old log
 */
int cfg_hub_key_get(uint8_t priv[CFG_ROOT_KEY_BYTES]);

/**
 * @brief The network hop key, from the identity sector.
 * @param key  receives it
 * @retval  0  an identity record exists and was copied out
 * @retval -1  none is stored
 *
 * Unlike the old log's accessor this never creates one, so reading it is a read.
 */
int cfg_net_key_get(uint8_t key[CFG_SESSION_BYTES]);

/**
 * @brief Appends an identity record. Never erases: the sector is written once.
 * @param priv  the hub's X25519 scalar
 * @param net   the network hop key
 * @return CFGF_OK, or why the flash refused
 *
 * The caller must witness the result before anything depends on it - the point
 * of §10's read, witness, then commit.
 */
cfgflash_err_t cfg_identity_write(const uint8_t priv[CFG_ROOT_KEY_BYTES],
                                  const uint8_t net[CFG_SESSION_BYTES]);

/** @brief The newest entry for a device. @param dev_id the device @return it, or NULL */
const cfg_device_t *cfg_find(uint32_t dev_id);

/** @brief Devices the roster holds. @return the count, tombstones excluded by design */
uint32_t cfg_live_devices(void);

/**
 * @brief Enrols a device at the lowest free uplink slot.
 * @param dev_id    the device
 * @param slot_out  receives the slot assigned; no operator ever picks one
 * @return CFGF_OK, or why it refused
 *
 * Re-enrolling an id keeps its slot, drops its keys and bumps key_gen.
 */
cfgflash_err_t cfg_enrol(uint32_t dev_id, uint8_t *slot_out);

/**
 * @brief Removes a device, freeing its entry and its slot.
 * @param dev_id  the device
 * @return CFGF_OK, or why it refused
 */
cfgflash_err_t cfg_forget(uint32_t dev_id);

/**
 * @brief Completes a pairing against an already-enrolled device.
 * @param dev_id        the device that paired
 * @param session       the session key, stored rather than re-derived
 * @param nonce         the last accepted nonce
 * @param rotate_epoch  the epoch the key belongs to
 * @param pubkey        the device's key as PAIR_REQ carried it
 * @return CFGF_OK, or why it refused
 */
cfgflash_err_t cfg_pair_complete(uint32_t dev_id, const uint8_t session[16],
                                 const uint8_t nonce[8], uint32_t rotate_epoch,
                                 const uint8_t pubkey[CFG_PUBKEY_BYTES]);

/**
 * @brief Appends one device entry as a delta, or a checkpoint when one is due.
 * @param dev  the entry; its header is stamped here
 * @return CFGF_OK, or why the flash refused
 */
cfgflash_err_t cfg_put_device(const cfg_device_t *dev);

/**
 * @brief Appends the config head as a delta, or a checkpoint when one is due.
 * @param cfg  the config to persist
 * @return CFGF_OK, or why the flash refused
 */
cfgflash_err_t cfg_put_config(const cfg_config_t *cfg);

#endif /* CFGSTOREAPI_H */
