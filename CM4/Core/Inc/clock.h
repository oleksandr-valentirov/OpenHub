#pragma once

#include <stdint.h>

#include "timebase.h"

/**
 * @file clock.h
 * @brief TIM2 free-running at a nominal 1 MHz, and the scale that corrects it.
 *
 * The four operations the grid rests on are Common/inc/timebase.h, which both
 * firmwares compile. This file is what is left: the tick source under that
 * seam, and the calibration policy above it, both of which are this hub's.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */

/**
 * @brief The free-running tick counter, wrapping every ~71.6 minutes.
 * @return TIM2's count, in ticks rather than microseconds
 *
 * timebase_now() is this, and is the name the grid uses.
 */
uint32_t rfm_micros(void);

/**
 * @brief Busy-waits, blocking the caller for the whole interval.
 * @param us  ticks to wait, not corrected microseconds
 */
void     delay_us_poll(uint32_t us);

/**
 * @brief Installs a new tick-per-microsecond scale, from an LSE window.
 * @param scale_q24  ticks per nominal microsecond, Q24
 */
void     timebase_set_scale(uint32_t scale_q24);

/**
 * @brief The scale in force, so a figure can carry the ppm that converted it.
 * @return ticks per nominal microsecond, Q24
 */
uint32_t timebase_scale(void);
