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

#endif /* BOOTWAIT_H */
