#pragma once

#include <stdint.h>

#include "dev_entry.h"

/**
 * @file dlsched.h
 * @brief Which device the one downlink of a superframe goes to.
 *
 * radio_devices_docs/open_hub/radio/superloop.md
 */

/**
 * @brief Whether this superframe is the device's own window, by the period in force.
 * @param d   the roster entry
 * @param sf  the superframe about to be served
 * @retval 1  the device opens a receiver in it
 * @retval 0  it does not, or the entry is unused
 */
int dl_own_window(const dev_entry_t *d, uint32_t sf);

/**
 * @brief Whether a downlink may be addressed to the device in this superframe.
 * @param d   the roster entry
 * @param sf  the superframe about to be served
 * @retval 1  its own window, or it is unheard and ADR-0023 may have muted it
 * @retval 0  neither
 */
int dl_due(const dev_entry_t *d, uint32_t sf);

/**
 * @brief Picks the device to serve and advances the rotation over its own window only.
 * @param devices  the roster
 * @param count    entries in it
 * @param next     the rotation cursor, read and written
 * @param sf       the superframe about to be served
 * @return the index served, or -1 when nobody is due
 */
int dl_pick(const dev_entry_t *devices, uint8_t count, uint8_t *next, uint32_t sf);
