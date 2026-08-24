#pragma once

#include <stdint.h>

/**
 * @file grid.h
 * @brief The absolute superframe grid: one boundary, one period, one counter.
 *
 * **The one rule a master and a slave share.** A grid steps from its last
 * boundary and never from now, and a stall that crossed several boundaries
 * counts every one of them - it is a schedule, not a queue. Written three
 * times across the two trees before this file existed, and the third copy was
 * in a reference implementation, where a divergence would have been least
 * visible.
 *
 * The unit is the caller's: ticks on the hub, microseconds on the device.
 * **Nothing here converts, and nothing here reads a clock** - the instant is
 * passed in, so this file has no backend at all and the role above chooses
 * which clock its grid rides. Nothing here decides the period either.
 *
 * radio_devices_docs/radio/tdma.md
 */
typedef struct {
    uint32_t start;    /**< the current boundary, on timebase_now()'s clock */
    uint32_t period;   /**< in the same unit; a zero period never advances */
    uint32_t counter;
    uint8_t  running;
} grid_t;

/**
 * @brief Starts the grid, with now as its first boundary.
 * @param g        the grid to start
 * @param counter  the superframe number this boundary carries
 * @param period   the interval, in the clock's own unit
 * @param now      the instant, on whichever clock this grid will ride
 */
void grid_start(grid_t *g, uint32_t counter, uint32_t period, uint32_t now);

/**
 * @brief Steps the grid to an instant.
 * @param g    a started grid
 * @param now  the instant, on the same clock grid_start was given
 * @return how many boundaries were crossed; 0 when none was
 *
 * A zero period returns 0 rather than looping forever - it is what an
 * unmeasured slave holds, and it must not be able to hang the caller.
 */
uint32_t grid_advance(grid_t *g, uint32_t now);

/**
 * @brief How far into the current superframe an instant falls.
 * @param g    a started grid
 * @param now  the instant, on the same clock
 * @return the offset from the current boundary, wrap-safe
 */
uint32_t grid_offset(const grid_t *g, uint32_t now);
