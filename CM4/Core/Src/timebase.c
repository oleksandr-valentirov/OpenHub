/**
 * @file timebase.c
 * @brief TIM2 free-runs at 1 MHz over 32 bits, wrapping every ~71.6 minutes.
 *
 * radio_devices_docs/open_hub/radio/timebase.md
 */
#include "main.h"
#include "timebase.h"

extern TIM_HandleTypeDef htim2;

static uint32_t delay_end_us = 0;
static volatile uint8_t delay_flag = 1;

/* Ticks per nominal microsecond, Q24, and nominal until the first LSE window. */
static volatile uint32_t scale_q24 = 1u << 24;

void timebase_set_scale(uint32_t q24) {
    scale_q24 = q24;
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

uint32_t get_rfm_counter(void) {
    return rfm_micros() / 1000u;
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

void delay_ms_poll(uint32_t ms) {
    delay_us_poll(ms * 1000u);
}

/* Non-blocking deadline the radio state machine polls through get_delay_ms_flag. */
void delay_ms_it(uint32_t ms) {
    delay_flag = 0;
    delay_end_us = rfm_micros() + ms * 1000u;
}

uint8_t get_delay_ms_flag(void) {
    if (!delay_flag && timebase_elapsed(delay_end_us)) {
        delay_flag = 1;
    }
    return delay_flag;
}
