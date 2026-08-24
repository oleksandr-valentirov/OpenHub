#pragma once

#include <stdint.h>

/**
 * @file timebase.h
 * @brief The clock the grid rests on: four operations, and nothing else.
 *
 * **One contract, two firmwares, no copy** - the same reason phy.h lives here.
 * Each firmware supplies all four; a firmware missing one does not link.
 *
 * The two conversions exist because the hub keeps its grid in TIM2 ticks and
 * scales them by the last LSE calibration window, while the device's TIM2
 * free-runs at a nominal 1 MHz and is disciplined against nothing. **On the
 * device both are the identity today and must still exist** - a backend that
 * omits an operation it does not currently need is a backend the library
 * cannot be built against, and the day the device disciplines its own clock
 * the seam is already here rather than being cut under a working link.
 *
 * What is deliberately absent, each for a reason: start and service are
 * lifecycle, uptime and millis are a firmware's own bookkeeping, the scale
 * getter and setter are the hub's calibration policy, and a blocking delay is
 * a wait the library must never take.
 *
 * radio_devices_docs/radio/decisions/0029-the-library-declares-four-backends-and-absorbs-no-control.md
 * radio_devices_docs/radio/decisions/0030-radio-stack-is-the-link-layer-and-the-session-layer-is-a-separate-consumer.md
 */

/**
 * @brief The backend's own clock, in the backend's own unit.
 * @return a free-running count; every caller must tolerate its wrap
 *
 * Ticks on the hub and microseconds on the device. Nothing above this may
 * assume which - that is what the two conversions are for.
 */
uint32_t timebase_now(void);

/**
 * @brief Whether a deadline has passed, correct across the counter's wrap.
 * @param deadline  a value previously derived from timebase_now()
 * @retval 1  the deadline is reached or passed
 * @retval 0  it is still ahead
 */
uint8_t  timebase_elapsed(uint32_t deadline);

/**
 * @brief Converts a wanted interval into the ticks that measure it.
 * @param us  microseconds wanted
 * @return the tick count on this backend's clock
 */
uint32_t timebase_us_to_ticks(uint32_t us);

/**
 * @brief Converts a measured tick span into microseconds.
 * @param ticks  a difference of two timebase_now() reads
 * @return the span in microseconds
 */
uint32_t timebase_ticks_to_us(uint32_t ticks);
