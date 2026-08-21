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
    uint8_t buf[IPC_PAYLOAD_MAX];
    uint32_t waited = 0;
    uint16_t seq = 0;
    int result = -1;

    if (reply == NULL || (uint32_t)len + 1u > sizeof(buf))
        return 1;
    if (ipc_lock == NULL)
        return 1;

    buf[0] = arg;
    if (payload != NULL && len > 0u)
        memcpy(&buf[1], payload, len);

    if (osMutexAcquire(ipc_lock, IPC_REPLY_TIMEOUT_MS) != osOK)
        return 1;

    if (ipc_send_request(type, buf, (uint8_t)(len + 1u), &seq) == 0) {
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
