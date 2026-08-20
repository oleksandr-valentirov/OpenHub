#pragma once

#include <stdint.h>

uint32_t rfm_micros(void);
uint32_t get_rfm_counter(void);
uint8_t  timebase_elapsed(uint32_t deadline_us);
void     delay_us_poll(uint32_t us);
void     delay_ms_poll(uint32_t ms);
void     delay_ms_it(uint32_t ms);
uint8_t  get_delay_ms_flag(void);

/* TIM2 ticks are only nominally microseconds: the clock they come from is off by
 * whatever calib.c measured against the LSE crystal. Anything scheduled on the
 * TDMA grid converts through here; timeouts can keep using raw ticks. */
uint32_t timebase_us_to_ticks(uint32_t us);
void     timebase_set_scale(uint32_t scale_q24);
uint32_t timebase_scale(void);
