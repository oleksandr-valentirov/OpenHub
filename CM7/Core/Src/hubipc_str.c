/**
 * @file hubipc_str.c
 * @brief The mailbox's failure renderer, alone, so a host test can reach it.
 *
 * hubipc.c includes cmsis_os.h for the mutex and this function needs none of it.
 * Two IPC failures once shared one number; these strings exist to separate the
 * cases, so no two of them may be equal.
 * radio_devices_docs/open_hub/arch/ipc.md
 */

#include "hubipc.h"

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
