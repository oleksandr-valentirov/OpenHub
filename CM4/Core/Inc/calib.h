#pragma once

#include <stdint.h>

/**
 * @file calib.h
 * @brief Measures TIM2 against LSE, so a tick can be converted to a microsecond.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */

/** @brief Starts the capture unit and takes one window. */
void     calib_init(void);

/** @brief Drains captures and accepts or rejects each window; call per superloop pass. */
void     calib_poll(void);

/**
 * @brief Whether a window has landed, so the scale is measured rather than nominal.
 * @retval 1  at least one window was accepted
 * @retval 0  the scale in force is still the reset default
 */
uint8_t  calib_ready(void);

/**
 * @brief The timer clock's offset from nominal.
 * @return signed ppm, positive when TIM2 runs fast
 */
int32_t  calib_ppm(void);

/**
 * @brief Low extreme of the spans inside the last window.
 * @return ticks
 */
uint32_t calib_span_lo(void);

/**
 * @brief High extreme of the spans inside the last window.
 * @return ticks
 */
uint32_t calib_span_hi(void);

/**
 * @brief Lowest ppm across windows; a wide spread means a bad measurement.
 * @return signed ppm
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */
int32_t  calib_ppm_min(void);

/**
 * @brief Highest ppm across windows, read together with calib_ppm_min().
 * @return signed ppm
 */
int32_t  calib_ppm_max(void);

/**
 * @brief Windows completed, so a ppm figure can be read as a population.
 * @return count since boot
 */
uint32_t calib_windows(void);

/**
 * @brief Windows the consistency checks dropped.
 * @return count since boot
 */
uint32_t calib_rejects(void);

/**
 * @brief Age of the last accepted window, because a stopped reference is silent.
 * @return ticks since it landed
 */
uint32_t calib_age_tk(void);

/**
 * @brief How stale the last window may be before the grid stops being held.
 *
 * Windows land about three times a second on this bench, so 10 s is thirty of
 * them. Overridable so a build can lower it and make the refusal fire on a
 * healthy crystal - the detector is worthless until it has been seen to.
 * radio_devices_docs/open_hub/radio/timebase.md
 */
#ifndef CALIB_STALE_US
#define CALIB_STALE_US  10000000u
#endif

/**
 * @brief The staleness rule alone, so a host test can reach it without the HAL.
 * @param age  what calib_age_tk() returned
 * @retval 1   recent enough to hold a grid on
 * @retval 0   stale, or 0xFFFFFFFF for a window that never landed
 *
 * Strict: at exactly CALIB_STALE_US the window is already too old, so the
 * comparison keeps carrying information at the value a reader would pick.
 */
static inline int calib_age_ok(uint32_t age) {
    return age < CALIB_STALE_US;
}

/**
 * @brief Whether the timebase is disciplined *now*, rather than ever having been.
 * @retval 1  the capture unit runs and a window landed recently
 * @retval 0  never started, never landed, or the reference has stopped
 *
 * calib_ready() is sticky by design and cannot answer this: one window at boot
 * and a dead crystal afterwards leaves it 1 for the life of the image.
 * radio_devices_docs/open_hub/radio/timebase.md
 */
uint8_t  calib_disciplined(void);
