#pragma once

#include <stdint.h>

/**
 * @file radio.h
 * @brief CM4's radio: the superframe grid, the frames on it, and the pairing window.
 *
 * radio_devices_docs/open_hub/radio/superloop.md
 */

/**
 * @brief Brings the SX1231 up and installs the settings the air interface asks for.
 * @param network_id  the network this hub belongs to
 * @param node_id     this hub's identifier on it
 * @retval 0   the part answered and is configured
 * @retval !=0 it did not; nothing below this point is trustworthy
 *
 * radio_devices_docs/open_hub/radio/configuration.md
 */
uint8_t RFM_Init(uint8_t network_id, uint8_t node_id);

/**
 * @brief One pass of the superloop: services the grid, the radio and the mailbox.
 *
 * Called from CM4's main loop and feeds IWDG2, so a pass that never returns is a
 * reset rather than a hang. radio_devices_docs/open_hub/radio/superloop.md
 */
void RFM_Routine(void);
