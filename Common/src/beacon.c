/* Data beacon reception: cleartext and unauthenticated, so version, length, content.
 * radio_devices_docs/wl55_device/radio/beacon.md */
#include <string.h>

#include "beacon.h"
#include "radio_phy.h"
#include "radio_protocol.h"
#include "radio_slots.h"

#define BEACON_VERSION  2u

/* Against the contract's literal: the length check below already uses sizeof.
 * radio_devices_docs/wl55_device/radio/beacon.md */
#define BEACON_V2_WIRE_BYTES  14u
_Static_assert(sizeof(radio_data_beacon_t) == BEACON_V2_WIRE_BYTES,
               "v2 data beacon must stay 14 bytes on the wire");

/* One gate: peeking must not be a looser route than applying. */
static beacon_rc_t beacon_parse(const uint8_t *frame, uint8_t len,
                                radio_data_beacon_t *b) {
    if (len < 2u || frame[0] != RADIO_FRAME_DATA_BEACON)
        return BEACON_NOT_BEACON;
    if (frame[1] != BEACON_VERSION)
        return BEACON_BAD_VERSION;
    if (len != sizeof(*b))
        return BEACON_BAD_LENGTH;
    memcpy(b, frame, sizeof(*b));
    return BEACON_OK;
}

beacon_rc_t beacon_peek(const uint8_t *frame, uint8_t len, uint32_t *superframe) {
    radio_data_beacon_t b;
    beacon_rc_t rc = beacon_parse(frame, len, &b);

    if (rc != BEACON_OK)
        return rc;
    if (superframe != NULL)
        *superframe = b.superframe;
    return BEACON_OK;
}

beacon_rc_t beacon_apply(const uint8_t *frame, uint8_t len,
                         superframe_t *sf, quiesce_t *q, uint32_t at_us,
                         uint32_t *aligned_to) {
    radio_data_beacon_t b;
    beacon_rc_t prc = beacon_parse(frame, len, &b);

    if (prc != BEACON_OK)
        return prc;

    /* Captured before aligning: success sets aligned and clears rejected.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    uint8_t  was_aligned    = sf->aligned;
    uint32_t prior_rejected = sf->rejected;

    /* The first bit is not the boundary: the hub's is upstream of its transmit.
     * radio_devices_docs/wl55_device/radio/timebase.md */
    int arc = superframe_align_at(sf, b.superframe,
                                  at_us - BEACON_BOUNDARY_LAG_US);
    if (arc == -1)
        return BEACON_STALE;
    if (arc == -2)
        return BEACON_SUSPECT;
    if (aligned_to != NULL)
        *aligned_to = b.superframe;

    /* Every accepted beacon is a tick, or resuming traffic never clears the flag.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    (void)quiesce_active(q, b.superframe);

    if (!(b.flags & RADIO_BEACON_FLAG_QUIESCE))
        return BEACON_OK;

    /* Was already aligned, not is aligned now: align sets OK on every success.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    if (!was_aligned || prior_rejected) {
        q->refused_sync++;
        return BEACON_OK;
    }

    uint8_t in = b.resume_in;
    if (in > RADIO_QUIESCE_SUPERFRAMES) {
        in = RADIO_QUIESCE_SUPERFRAMES;
        q->clamped++;
    }
    uint32_t resume_at = b.superframe + in;

    if (q->active) {
        /* The earlier resume: the hub commits at announce time and never extends.
         * radio_devices_docs/wl55_device/radio/beacon.md */
        if ((int32_t)(resume_at - q->resume_at) < 0)
            q->resume_at = resume_at;
        return BEACON_OK;
    }

    /* The clamp bounds one forgery; requiring traffic between caps quiesce at half.
     * radio_devices_docs/wl55_device/radio/beacon.md */
    uint32_t since = superframe_now(sf) - q->last_resume;
    if (q->ever && since < RADIO_QUIESCE_MIN_GAP) {
        q->refused_gap++;
        return BEACON_OK;
    }

    q->active = 1;
    q->resume_at = resume_at;
    return BEACON_OK;
}

int quiesce_active(quiesce_t *q, uint32_t superframe) {
    if (!q->active)
        return 0;
    /* Signed difference, so the comparison survives the counter wrapping. */
    if ((int32_t)(superframe - q->resume_at) >= 0) {
        q->active = 0;
        q->last_resume = q->resume_at;
        q->ever = 1;
        return 0;
    }
    return 1;
}

const char *beacon_rc_name(beacon_rc_t rc) {
    switch (rc) {
    case BEACON_OK:          return "ok";
    case BEACON_NOT_BEACON:  return "not a data beacon";
    case BEACON_BAD_VERSION: return "unknown version";
    case BEACON_BAD_LENGTH:  return "wrong length for this version";
    case BEACON_STALE:       return "would reuse a counter; re-pair";
    case BEACON_SUSPECT:     return "implausible jump; treated as a forgery";
    default:                 return "?";
    }
}
