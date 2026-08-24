#pragma once

/**
 * @file main.h
 * @brief The board, as much of it as phy_rfm69.c touches, for a host build.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stddef.h>
#include <stdint.h>

#define GPIO_PIN_RESET  0
#define GPIO_PIN_SET    1

#define RFM_RESET_Pin        0x1000u
#define RFM_RESET_GPIO_Port  ((void *)1)
#define RFM_DIO3_Pin         0x0100u
#define RFM_CS_Pin           0x8000u
#define RFM_CS_GPIO_Port     ((void *)2)

void HAL_GPIO_WritePin(void *port, uint16_t pin, int state);
int  rfm_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len);
/* The real one is HAL's weak symbol; phy_rfm69.c defines it on both builds. */
void HAL_GPIO_EXTI_Callback(uint16_t pin);
