#pragma once

#include <stdint.h>

#include "ipc.h"

/**
 * @file hubipc.h
 * @brief CM7's half of the mailbox, serialised across the whole transaction.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */

/** @brief Creates the mutex every CM7 caller is serialised by. */
void hub_ipc_init(void);

/**
 * @brief Sends one request and waits for the reply carrying its sequence number.
 * @param type     the IPC_REQ_* being asked for
 * @param arg      travels as payload byte 0 when @p payload is NULL
 * @param payload  the request body, or NULL
 * @param len      its length, at most IPC_PAYLOAD_MAX
 * @param reply    receives CM4's answer
 * @retval  0  the reply arrived and CM4 reported success
 * @retval -1  no reply inside the timeout
 * @return otherwise CM4's own IPC status
 *
 * Holds the mutex across send and wait, not just the send: two pollers drain
 * each other's replies. radio_devices_docs/open_hub/arch/ipc.md
 */
int hub_ipc_call(uint8_t type, uint8_t arg, const void *payload, uint8_t len,
                 ipc_msg_t *reply);
