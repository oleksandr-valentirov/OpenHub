#pragma once

#include <stdint.h>

#include "ipc.h"

/**
 * @file hubipc.h
 * @brief CM7's half of the mailbox, serialised across the whole transaction.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */

/* CM7's own, and negative: CM4's statuses are 0..3 and were indistinguishable.
 * radio_devices_docs/open_hub/arch/ipc.md */
#define HUB_IPC_NO_REPLY  (-1)   /**< sent, and no reply inside the timeout */
#define HUB_IPC_ERR_ARG   (-2)   /**< the call was malformed before it left */
#define HUB_IPC_ERR_INIT  (-3)   /**< hub_ipc_init() has not run */
#define HUB_IPC_ERR_BUSY  (-4)   /**< another CM7 caller held the mutex */
#define HUB_IPC_ERR_SEND  (-5)   /**< the mailbox would not take the request */

/** @brief Creates the mutex every CM7 caller is serialised by. */
void hub_ipc_init(void);

/**
 * @brief Renders any hub_ipc_call() return as a sentence for a console.
 * @param rc  what hub_ipc_call() returned
 * @return a static string, never NULL
 */
const char *hub_ipc_str(int rc);

/**
 * @brief Sends one request and waits for the reply carrying its sequence number.
 * @param type     the IPC_REQ_* being asked for
 * @param arg      travels as payload byte 0 when @p payload is NULL
 * @param payload  the request body, or NULL
 * @param len      its length, at most IPC_PAYLOAD_MAX
 * @param reply    receives CM4's answer
 * @retval  0  the reply arrived and CM4 reported success
 * @retval <0  HUB_IPC_*: this side failed and CM4 was never asked, except
 *             HUB_IPC_NO_REPLY, where it was asked and did not answer
 * @return otherwise CM4's own IPC status, which is positive
 *
 * Holds the mutex across send and wait, not just the send: two pollers drain
 * each other's replies. radio_devices_docs/open_hub/arch/ipc.md
 */
int hub_ipc_call(uint8_t type, uint8_t arg, const void *payload, uint8_t len,
                 ipc_msg_t *reply);
