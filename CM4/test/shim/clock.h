#pragma once

/**
 * @file clock.h
 * @brief The clock phy_rfm69.c reads, driven by the test rather than by TIM2.
 *
 * Mirrors the real CM4/Core/Inc/clock.h in shape: the seam below is the real
 * Common/inc/timebase.h, and only the tick source is stood in for.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdint.h>

#include "timebase.h"

uint32_t rfm_micros(void);
void     delay_us_poll(uint32_t us);
