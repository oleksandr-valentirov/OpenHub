#pragma once

#include <stdint.h>

#include "ipc.h"

/* Serialises CM7's half of the mailbox.
 *
 * The rings are single-producer by construction and CM7 now has two tasks that
 * want to talk to CM4 - the console and the pairing service. Two producers on
 * one ring is not the whole hazard: ipc_poll_reply *drains* the reply ring and
 * discards anything that does not match its sequence number, so a second poller
 * eats the first one's answer and both time out. The mutex therefore covers the
 * entire transaction, not just the send.
 *
 * The reply goes into a caller-owned buffer rather than a shared static. With a
 * static, the lock is released before the caller reads it, so the value can be
 * overwritten between the two - which is safe only for as long as there is
 * exactly one caller, and adding the second is what this file is for. */
void hub_ipc_init(void);

/* 0 on success, -1 when nothing answered in time, otherwise CM4's IPC status.
 * `arg` travels as payload byte 0, so every request has one shape on the wire. */
int hub_ipc_call(uint8_t type, uint8_t arg, const void *payload, uint8_t len,
                 ipc_msg_t *reply);
