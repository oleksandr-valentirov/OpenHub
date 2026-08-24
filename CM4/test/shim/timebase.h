#pragma once

/**
 * @file timebase.h
 * @brief The clock phy_rfm69.c reads, driven by the test rather than by TIM2.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdint.h>

uint32_t rfm_micros(void);
void     delay_us_poll(uint32_t us);
