/* The downlink's one seat a superframe, and who sits in it.
 * radio_devices_docs/open_hub/radio/superloop.md */
#include <stddef.h>

#include "dlsched.h"

#include "radio_protocol.h"

int dl_own_window(const dev_entry_t *d, uint32_t sf) {
    if (!d->used)
        return 0;
    if (d->every_now == 0u)
        return 1;
    if ((sf % d->every_now) == 0u)
        return 1;
    /* An applied rate whose ack was lost leaves the two sides on different periods.
     * radio_devices_docs/open_hub/radio/superloop.md */
    if (d->dl_cmd == RADIO_CMD_SET_RATE && !d->dl_acked &&
        d->dl_report_every != 0u && (sf % d->dl_report_every) == 0u)
        return 1;
    return 0;
}

int dl_due(const dev_entry_t *d, uint32_t sf) {
    if (!d->used)
        return 0;
    /* Unheard this boot: no period, and it may be the one ADR-0023 has muted.
     * radio_devices_docs/open_hub/radio/superloop.md */
    if (d->frames_ok == 0u)
        return 1;
    return dl_own_window(d, sf);
}

int dl_pick(const dev_entry_t *devices, uint8_t count, uint8_t *next, uint32_t sf) {
    uint8_t i;

    if (devices == NULL || next == NULL || count == 0u)
        return -1;
    for (i = 0u; i < count; i++) {
        uint8_t slot = (uint8_t)((*next + i) % count);

        if (dl_due(&devices[slot], sf)) {
            /* A serve outside the device's own window must not spend its turn.
             * radio_devices_docs/open_hub/radio/superloop.md */
            if (dl_own_window(&devices[slot], sf))
                *next = (uint8_t)((slot + 1u) % count);
            return (int)slot;
        }
    }
    return -1;
}
