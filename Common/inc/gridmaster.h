#pragma once

#include <stdint.h>

#include "grid.h"

/**
 * @file gridmaster.h
 * @brief The role that defines the grid rather than tracking one.
 *
 * A master owns the period: it is not measured, it is stated. What is left
 * over the shared rule is the bootstrap, and installing a period that moved -
 * on the hub the LSE calibration moves it, and it must move the **next**
 * interval rather than the one being closed.
 *
 * The slave role over the same rule is superframe.h.
 *
 * radio_devices_docs/radio/tdma.md
 */
typedef struct {
    grid_t  g;
    uint8_t started;
} gridmaster_t;

/**
 * @brief Starts the grid on the first call, and steps it on every one after.
 * @param m       the master's state, zeroed before the first call
 * @param now     the instant, on whichever clock this grid rides
 * @param period  the interval in force, in the clock's own unit, re-read by
 *                the caller every pass so a moved scale arrives here
 * @retval 1  a boundary was crossed and the per-superframe work is due
 * @retval 0  no boundary, or this call was the bootstrap
 *
 * **The counter the grid starts on is m->g.counter, whatever the caller left
 * there** - the hub seeds it from what flash guarantees and a fixture from a
 * build constant, and neither is this file's business.
 *
 * **One is returned once, however many boundaries a stall crossed**, because
 * the work due at a boundary is a schedule and not a queue. The counter still
 * counts every one of them.
 */
int gridmaster_service(gridmaster_t *m, uint32_t now, uint32_t period);
