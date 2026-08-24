#pragma once

#include <stdint.h>

#include "superframe.h"
#include "radio_slots.h"

/* Beacon reception in one place, so the CLI cannot skip a check by accident.
 * radio_devices_docs/wl55_device/radio/beacon.md */

typedef enum {
    BEACON_OK = 0,
    BEACON_NOT_BEACON,      /* some other frame type */
    BEACON_BAD_VERSION,
    BEACON_BAD_LENGTH,
    BEACON_STALE,           /* below the durable floor - the key must be replaced */
    BEACON_SUSPECT          /* a jump too large to believe */
} beacon_rc_t;

/* RADIO_QUIESCE_MIN_GAP: accepting less than the hub allows itself refuses it.
 * radio_devices_docs/wl55_device/radio/beacon.md */

typedef struct {
    uint8_t  active;
    uint32_t resume_at;     /* superframe the hub committed to resuming at */
    uint32_t last_resume;   /* when the previous quiesce ended, for the gap rule */
    uint8_t  ever;          /* last_resume is meaningless until the first one */
    uint32_t clamped;       /* announcements asking for more than the clamp */
    uint32_t refused_gap;   /* announcements refused for arriving too soon */
    uint32_t refused_sync;  /* announcements ignored because the clock was not trusted */
} quiesce_t;

/* How late a beacon-derived boundary reads against the hub's grid.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define BEACON_BOUNDARY_LAG_US  260u   /* +/- 5 */

/** @brief Reads the counter a beacon claims, before anything aligns to it.
 *  radio_devices_docs/radio/joining.md */
beacon_rc_t beacon_peek(const uint8_t *frame, uint8_t len, uint32_t *superframe);

/** @brief Parses, aligns and applies a quiesce.
 *  @param at_us the arrival instant */
beacon_rc_t beacon_apply(const uint8_t *frame, uint8_t len,
                         superframe_t *sf, quiesce_t *q, uint32_t at_us, uint32_t *aligned_to);

/** @brief Whether the grid is suspended now; also retires an expired quiesce. */
int quiesce_active(quiesce_t *q, uint32_t superframe);

/** @brief Names a beacon result for the console. */
const char *beacon_rc_name(beacon_rc_t rc);
