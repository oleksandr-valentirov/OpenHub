#ifndef HUBCONFIG_H
#define HUBCONFIG_H

#include <stdint.h>

#include "cfgflash.h"

/**
 * @file hubconfig.h
 * @brief The seam between the stored configuration and the things that use it.
 *
 * One module applies a setting and persists it, in that order, and replays the
 * stored ones at boot. ROADMAP item 38.
 * radio_devices_docs/open_hub/arch/config-store.md
 */

/**
 * @brief Applies everything the store holds. Called once, after the netif exists.
 *
 * A setting that was never stored is left alone rather than applied as zero.
 */
void hubconfig_apply_boot(void);

/**
 * @brief Points the northbound link at a server and remembers it.
 * @param ip     dotted-quad, as the operator typed it
 * @param port   1..65535
 * @param token  optional bearer, or NULL
 * @retval  0        applied and persisted
 * @retval -1        the address did not parse; nothing was applied
 * @retval -2        applied, but the store refused it - it will not survive a reset
 */
int hubconfig_set_server(const char *ip, uint16_t port, const char *token);

/**
 * @brief Moves the interface to a fixed address and remembers it.
 * @param ip    the address
 * @param mask  the netmask
 * @param gw    the gateway
 * @retval  0   applied and persisted
 * @retval -2   applied, but the store refused it
 */
int hubconfig_set_static(uint32_t ip, uint32_t mask, uint32_t gw);

/**
 * @brief Hands the interface back to DHCP and remembers that too.
 * @retval  0   applied and persisted
 * @retval -2   applied, but the store refused it
 */
int hubconfig_set_dhcp(void);

/**
 * @brief Sets how many missed reports in a row make a device's link lost.
 * @param misses  1..255
 * @retval  0   persisted
 * @retval -1   out of range
 * @retval -2   the store refused it
 */
int hubconfig_set_link_lost(uint8_t misses);

/** @brief The threshold in force. @return the stored value, or the default */
uint8_t hubconfig_link_lost(void);

/** @brief Why the last append refused, or CFGF_OK. @return the code */
cfgflash_err_t hubconfig_last_err(void);

#endif /* HUBCONFIG_H */
