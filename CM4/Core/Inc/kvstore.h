#pragma once

#include <stdint.h>

/* Durable storage for the one value that must survive a reset: the superframe
 * counter. See CM4/Core/Src/kvstore.c for why it stores a ceiling rather than
 * the counter itself. */

/* Scans flash and recovers the reserved ceiling. Must run before the watchdog
 * is started: recovery may erase a sector, and a 128 KB erase can outlast the
 * 512 ms IWDG period. Returns 0 on success. */
uint8_t  kv_init(void);

/* The counter value the radio must start at. Nothing at or below this has been
 * durably reserved by a previous boot, so nothing below it can be reused. */
uint32_t kv_reserved(void);

/* Called every superframe. Writes only when the counter approaches the ceiling,
 * which is once per KV_RESERVE_AHEAD superframes. Returns 0 on success. */
uint8_t  kv_reserve(uint32_t counter);

/* 1 while `counter` is at or below what flash guarantees. When this goes false
 * the hub must not transmit: a frame sealed with a counter beyond the stored
 * ceiling uses a nonce a future boot will hand out again. */
uint8_t  kv_counter_safe(uint32_t counter);

/* Test scaffolding: see kvstore.c. Returns 0 if the bad record was written. */
int kv_write_torn(void);

uint32_t kv_writes(void);
uint32_t kv_errors(void);
uint32_t kv_slots_left(void);
