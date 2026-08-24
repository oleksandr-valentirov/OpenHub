#pragma once

#include <stdint.h>

#include "rfm69.h"

/**
 * @file phy_rfm69.h
 * @brief Scaffolding while radio.c still calls the driver directly.
 *
 * Nothing here is part of the PHY contract. `phy.h` is the contract, and the
 * whole point of the seam is that the layer above never learns the chip has
 * registers - so **every declaration in this file is a debt**, and the file is
 * deleted when the last operation site in radio.c moves behind `phy.h`.
 *
 * OpenHub/ROADMAP.md item 75
 * radio_devices_docs/radio/phy-seam.md
 */

/* What phy_init() maps DIO3 to; radio.c only reports it back. */
#define RFM69_DIO3_SYNC_ADDRESS  2u

/** @brief The driver handle, for the operation sites not yet behind phy.h. */
rfm69_dev_t *phy_rfm69_dev(void);

/** @brief RegDioMapping1 as read back after configuration, for IPC_REQ_GET_SYNCTIME. */
uint8_t phy_rfm69_dio_map1(void);
