#pragma once

#include <stdint.h>

#include "grid.h"
#include "radio_slots.h"

/* The slave role over grid.h's rule; the master role is gridmaster.h.
 * radio_devices_docs/wl55_device/radio/timebase.md */

/* Deliberately not 2000 ms, so anything depending on the nominal fails here.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_STUB_US  1900000u

/* One day of slack: more than a device in range needs, less than a forger wants.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_MAX_JUMP  43200u

/* Aligned must expire, or the state reports the last event and not validity.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_FRESH_US  (2u * SUPERFRAME_US)

/* Refusals after which the device gives up its own alignment. Not a weakening.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_RESYNC_AFTER  8u

/* Clamping trusts the hub less: a lie moves the estimate 1%, not the window.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_PERIOD_MIN_US \
    (SUPERFRAME_US - SUPERFRAME_US / 100u * SUPERFRAME_PERIOD_TOL_PCT)
#define SUPERFRAME_PERIOD_MAX_US \
    (SUPERFRAME_US + SUPERFRAME_US / 100u * SUPERFRAME_PERIOD_TOL_PCT)

/* Superframes the period is measured across: one timestamp's noise divides by it.
 * radio_devices_docs/wl55_device/radio/timebase.md */
#define SUPERFRAME_PERIOD_BASELINE      64u
/* Past this the elapsed microseconds wrap, so the span is re-anchored unmeasured. */
#define SUPERFRAME_PERIOD_BASELINE_MAX  1024u

typedef enum {
    SF_SYNC_NONE = 0,      /* free-running, never heard a beacon */
    SF_SYNC_OK,            /* aligned to a beacon */
    SF_SYNC_SUSPECT,       /* a beacon asked for a jump too large to believe */
    SF_SYNC_STALE          /* a beacon would move the counter backwards */
} sf_sync_t;

typedef struct {
    grid_t   g;             /**< the counter, the period and the boundary */
    uint32_t last_beacon_us;
    uint8_t  aligned;
    sf_sync_t state;
    uint32_t rejected;      /* beacons refused since the last good one */
    uint32_t refused_counter; /**< the value refused, so the direction is not derived */
    int32_t  refused_jump;  /* signed: behind and far ahead are not one case */
    uint32_t prev_beacon_us;
    uint32_t prev_counter;
    uint32_t measured_us;   /* 0 until two beacons have been seen */
    uint32_t ref_beacon_us; /**< the far end of the span the period is measured over */
    uint32_t ref_counter;
    uint8_t  have_ref;
    uint8_t  have_prev;
} superframe_t;

/** @brief Starts the clock; a zero period is substituted with the stub. */
void     superframe_start(superframe_t *sf, uint32_t counter, uint32_t period_us);

/** @brief Steps the counter to now and returns it. */
uint32_t superframe_now(superframe_t *sf);

/** @brief Aligns to a beacon; -2 is a jump too far to believe. */
int      superframe_align(superframe_t *sf, uint32_t counter);

/** @brief Aligns to a beacon that arrived at a known instant.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
int      superframe_align_at(superframe_t *sf, uint32_t counter,
                            uint32_t at_us);

/** @brief Names the sync state for the console. */
const char *superframe_state_name(const superframe_t *sf);

/** @brief Microseconds since the last accepted beacon. */
uint32_t superframe_since_beacon_us(const superframe_t *sf);

/** @brief Aligned, fresh, and on a measured period rather than the stub.
 *  radio_devices_docs/wl55_device/radio/timebase.md */
int      superframe_can_schedule(const superframe_t *sf);

/** @brief Whether the last beacon is inside SUPERFRAME_FRESH_US. */
int      superframe_is_fresh(const superframe_t *sf);
