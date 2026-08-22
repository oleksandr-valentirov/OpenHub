#pragma once

#include <stdint.h>

/**
 * @file timebase.h
 * @brief TIM2 free-running at a nominal 1 MHz, and the scale that corrects it.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */

/**
 * @brief The free-running tick counter, wrapping every ~71.6 minutes.
 * @return TIM2's count, in ticks rather than microseconds
 */
uint32_t rfm_micros(void);

/**
 * @brief Whether a deadline has passed, comparing signed so a wrap is safe.
 * @param deadline_us  a value previously derived from rfm_micros()
 * @retval 1  the deadline is reached or passed
 * @retval 0  it is still ahead
 */
uint8_t  timebase_elapsed(uint32_t deadline_us);

/**
 * @brief Busy-waits, blocking the caller for the whole interval.
 * @param us  ticks to wait, not corrected microseconds
 */
void     delay_us_poll(uint32_t us);

/**
 * @brief Converts a wanted interval into the ticks that measure it.
 * @param us  microseconds wanted
 * @return the tick count, scaled by the last calibration
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */
uint32_t timebase_us_to_ticks(uint32_t us);

/**
 * @brief Converts a measured tick span into microseconds.
 * @param ticks  a difference of two rfm_micros() reads
 * @return the span in microseconds, scaled by the last calibration
 */
uint32_t timebase_ticks_to_us(uint32_t ticks);

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
