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
