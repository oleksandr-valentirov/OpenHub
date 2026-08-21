#pragma once

#include <stdint.h>

/**
 * @file kvstore.h
 * @brief Durable storage for the superframe counter, held as a reserved ceiling.
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */

/**
 * @brief Recovers the reserved ceiling by scanning the log; run before the watchdog.
 * @retval 0  always, so a scan that found nothing is still a usable empty store
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */
uint8_t  kv_init(void);

/**
 * @brief The counter the radio must start at.
 * @return the recovered ceiling; everything below it was reserved by an earlier boot
 */
uint32_t kv_reserved(void);

/**
 * @brief Advances the ceiling, writing once per KV_RESERVE_AHEAD superframes.
 * @param counter  the superframe about to be used
 * @retval 0  always; kv_errors() carries whether the write itself failed
 */
uint8_t  kv_reserve(uint32_t counter);

/**
 * @brief Whether a superframe is inside what flash guarantees.
 * @param counter  the superframe about to be sealed under
 * @retval 1  safe to transmit
 * @retval 0  past the ceiling: silence, because sending here repeats a nonce
 *
 * radio_devices_docs/open_hub/arch/keystore.md
 */
uint8_t  kv_counter_safe(uint32_t counter);

/**
 * @brief Test scaffolding: writes a deliberately torn record for the scanner to skip.
 * @retval  0  the bad record reached flash
 * @retval !=0 it did not, so the test proves nothing
 */
int kv_write_torn(void);

/**
 * @brief Records written since boot.
 * @return count
 */
uint32_t kv_writes(void);

/**
 * @brief Writes that failed, counted apart so a silent store is distinguishable.
 * @return count
 */
uint32_t kv_errors(void);

/**
 * @brief Room left in the log, which is what the exhausted path is gated on.
 * @return records still writable
 */
uint32_t kv_slots_left(void);
