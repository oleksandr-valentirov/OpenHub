/**
 * @file hsem_table.h
 * @brief The single hardware-semaphore allocation table; nothing may take an unlisted one.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */
#pragma once

#define HSEM_ID_0   (0U)  /**< boot handshake: CM7 holds it, CM4 sleeps on it */

#define HSEM_RNG            (1U)    /**< guards RNG_DR, which either core can reach */
#define HSEM_M7_TO_M4_RFM   (2U)    /**< CM7 has filled the mailbox with a request */
#define HSEM_M4_TO_M7       (3U)    /**< CM4 has answered */
