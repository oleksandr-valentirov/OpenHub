/**
 * @file bootwait.h
 * @brief CM4's wait for CM7 to release the boot semaphore.
 */
#ifndef BOOTWAIT_H
#define BOOTWAIT_H

#include <stdint.h>
#include "main.h"

/**
 * @brief Spins until CM7 releases HSEM_ID_0, refreshing @p iwdg throughout.
 * @param iwdg the already-initialised IWDG2 handle, or NULL to skip the refresh
 */
void bootwait_for_cm7(IWDG_HandleTypeDef *iwdg);

/**
 * @brief How long the wait took.
 * @return milliseconds spent in bootwait_for_cm7(), 0 before it has run
 */
uint32_t bootwait_ms(void);

/**
 * @brief Passes the wait made, which is how many times the watchdog was refreshed.
 * @return the count; zero beside zero milliseconds means the wait never happened
 *
 * Without it a wait shorter than a tick and a wait that did not occur read alike.
 * radio_devices_docs/open_hub/arch/dual-core.md
 */
uint32_t bootwait_spins(void);

#endif /* BOOTWAIT_H */
