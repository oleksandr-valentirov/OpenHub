/**
 * @file test_ipc.c
 * @brief The layout two separately flashed cores agree on, and the arg's own home.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */

#include <stdio.h>
#include <stddef.h>
#include "ipc.h"

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

int main(void) {
    /* Change one and IPC_VERSION must change too, or two cores disagree. */
    CHECK(offsetof(ipc_msg_t, seq)     == 0u);
    CHECK(offsetof(ipc_msg_t, type)    == 2u);
    CHECK(offsetof(ipc_msg_t, status)  == 3u);
    CHECK(offsetof(ipc_msg_t, len)     == 4u);
    CHECK(offsetof(ipc_msg_t, arg)     == 5u);
    CHECK(offsetof(ipc_msg_t, payload) == 8u);

    /* The arg has storage of its own, so no payload offset reaches it.
     * ROADMAP item 24 */
    CHECK(offsetof(ipc_msg_t, arg) < offsetof(ipc_msg_t, payload));
    CHECK(sizeof(ipc_msg_t) == offsetof(ipc_msg_t, payload) + IPC_PAYLOAD_MAX);

    /* Both rings are copied word at a time, so the message must stay word sized. */
    CHECK(sizeof(ipc_msg_t) % 4u == 0u);
    CHECK(offsetof(ipc_msg_t, payload) % 4u == 0u);

    /* Parallel arrays and not an array of pairs, because an inner struct does
     * not inherit packed. radio_devices_docs/open_hub/radio/configuration.md */
    CHECK(offsetof(ipc_afc_raw_t, slot) ==
          offsetof(ipc_afc_raw_t, grid) + IPC_AFC_RING);
    CHECK(offsetof(ipc_afc_raw_t, gain) ==
          offsetof(ipc_afc_raw_t, slot) + IPC_AFC_RING);
    CHECK(offsetof(ipc_afc_raw_t, rssi) ==
          offsetof(ipc_afc_raw_t, gain) + IPC_AFC_RING);
    CHECK(offsetof(ipc_afc_raw_t, afc_hz) ==
          offsetof(ipc_afc_raw_t, rssi) + IPC_AFC_RING);
    CHECK(sizeof(ipc_afc_raw_t) ==
          offsetof(ipc_afc_raw_t, afc_hz) + 4u * IPC_AFC_RING);
    /* Hertz below the seam, so nothing above it holds an RFM69 register unit.
     * radio_devices_docs/radio/phy-seam.md */
    CHECK(sizeof(((ipc_afc_raw_t *)0)->afc_hz[0]) == 4u);

    /* The largest reply the mailbox carries, printed rather than assumed. */
    CHECK(sizeof(ipc_afc_raw_t) <= IPC_PAYLOAD_MAX);

    if (fails == 0)
        printf("ipc: ok (v%u, %u-byte message, payload at %u, "
               "largest reply %u of %u)\n",
               (unsigned)IPC_VERSION, (unsigned)sizeof(ipc_msg_t),
               (unsigned)offsetof(ipc_msg_t, payload),
               (unsigned)sizeof(ipc_afc_raw_t), (unsigned)IPC_PAYLOAD_MAX);
    return fails != 0;
}
