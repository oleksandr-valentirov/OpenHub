/**
 * @file hubipc.c
 * @brief CM7's half of the mailbox, serialised across the whole transaction.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */
#include <string.h>

#include "cmsis_os.h"
#include "hubipc.h"

#define IPC_REPLY_TIMEOUT_MS  500u

static osMutexId_t ipc_lock;

void hub_ipc_init(void) {
    ipc_lock = osMutexNew(NULL);
}

int hub_ipc_call(uint8_t type, uint8_t arg, const void *payload, uint8_t len,
                 ipc_msg_t *reply) {
    uint32_t waited = 0;
    uint16_t seq = 0;
    int result = HUB_IPC_NO_REPLY;

    /* All three returned 1, which is also IPC_ST_UNKNOWN_REQ.
     * radio_devices_docs/open_hub/arch/ipc.md */
    if (reply == NULL || len > IPC_PAYLOAD_MAX)
        return HUB_IPC_ERR_ARG;
    if (ipc_lock == NULL)
        return HUB_IPC_ERR_INIT;

    if (osMutexAcquire(ipc_lock, IPC_REPLY_TIMEOUT_MS) != osOK)
        return HUB_IPC_ERR_BUSY;

    if (ipc_send_request(type, arg, payload, len, &seq) != 0) {
        result = HUB_IPC_ERR_SEND;
    } else {
        while (waited < IPC_REPLY_TIMEOUT_MS) {
            if (ipc_poll_reply(seq, reply)) {
                result = reply->status;
                break;
            }
            osDelay(5);
            waited += 5;
        }
    }

    osMutexRelease(ipc_lock);
    return result;
}

const char *hub_ipc_str(int rc) {
    switch (rc) {
    case IPC_ST_OK:            return "ok";
    case IPC_ST_UNKNOWN_REQ:   return "CM4 does not know this request";
    case IPC_ST_BAD_ARG:       return "CM4 refused the arguments";
    case IPC_ST_RADIO_ERR:     return "CM4 reached the radio and it failed";
    case HUB_IPC_NO_REPLY:     return "CM4 did not answer inside the timeout";
    case HUB_IPC_ERR_ARG:      return "malformed on this core; CM4 was not asked";
    case HUB_IPC_ERR_INIT:     return "the mailbox was never initialised";
    case HUB_IPC_ERR_BUSY:     return "another CM7 caller held the mailbox";
    case HUB_IPC_ERR_SEND:     return "the mailbox would not take the request";
    default:                   return "an unknown status";
    }
}
