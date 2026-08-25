/**
 * @file timebase.c
 * @brief TIM2 free-runs at 1 MHz over 32 bits, wrapping every ~71.6 minutes.
 *
 * This is the hub's backend for Common/inc/timebase.h as well as its own
 * clock: all four of that seam's operations are supplied here.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */
#include "main.h"
#include "clock.h"

extern TIM_HandleTypeDef htim2;

/* Ticks per nominal microsecond, Q24, and nominal until the first LSE window. */
static volatile uint32_t scale_q24 = 1u << 24;
static volatile uint32_t scale_refused;

/* Refused rather than clamped, and counted rather than silent.
 * radio_devices_docs/open_hub/radio/timebase.md */
void timebase_set_scale(uint32_t q24) {
    if (q24 == 0u) {
        scale_refused++;
        return;
    }
    scale_q24 = q24;
}

uint32_t timebase_scale_refused(void) {
    return scale_refused;
}

uint32_t timebase_scale(void) {
    return scale_q24;
}

uint32_t timebase_us_to_ticks(uint32_t us) {
    return (uint32_t)(((uint64_t)us * scale_q24) >> 24);
}

uint32_t timebase_ticks_to_us(uint32_t ticks) {
    return (uint32_t)(((uint64_t)ticks << 24) / scale_q24);
}

uint32_t rfm_micros(void) {
    return __HAL_TIM_GET_COUNTER(&htim2);
}

/* The seam's name for the clock the grid already ran on. ADR-0029 decision 3. */
uint32_t timebase_now(void) {
    return rfm_micros();
}

/* Signed difference, so a deadline straddling the wrap still compares right. */
uint8_t timebase_elapsed(uint32_t deadline_us) {
    return (int32_t)(rfm_micros() - deadline_us) >= 0;
}

void delay_us_poll(uint32_t us) {
    const uint32_t end = rfm_micros() + us;
    while (!timebase_elapsed(end)) {
        __asm__("nop");
    }
}
