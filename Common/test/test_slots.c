/* The slot grid is a hub/device contract, and every number in it is derived
 * from another number. A constant edited without re-deriving the rest is the
 * failure this file exists to catch: it does not check that the geometry is
 * good, it checks that it is still consistent with itself and still inside the
 * duty-cycle allowance. */

#include <stdio.h>
#include <string.h>
#include "radio_slots.h"
#include "radio_phy.h"
#include "radio_protocol.h"

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* What the hub may transmit in one superframe, in microseconds of air time. */
#define BEACON_AIR   RADIO_AIRTIME_US(sizeof(radio_data_beacon_t))
#define JOIN_AIR     RADIO_AIRTIME_US(sizeof(radio_join_beacon_t))
#define DOWNLINK_AIR RADIO_AIRTIME_US(28u)   /* 12 B header + 16 B GCM tag */

int main(void) {
    /* The join channel, from the grid arithmetic rather than as a literal. The
     * hub's RegFrf reads 0xD8A000 for this slot, which is exactly 866.5 MHz. */
    CHECK(RADIO_JOIN_HZ == 866500000u);
    CHECK(RADIO_SLOT_HZ(0) == RADIO_CH_BASE_HZ);
    /* The mapping must step over the reserved slot, not through it. */
    CHECK(RADIO_HOP_TO_GRID(RADIO_JOIN_SLOT - 1u) == RADIO_JOIN_SLOT - 1u);
    CHECK(RADIO_HOP_TO_GRID(RADIO_JOIN_SLOT) == RADIO_JOIN_SLOT + 1u);
    CHECK(RADIO_HOP_TO_GRID(RADIO_HOP_COUNT - 1u) == RADIO_GRID_COUNT - 1u);
    /* Air time from the PHY fields against the slot geometry's own literal. */
    CHECK(RADIO_FRAME_AIR_US(RADIO_UPLINK_BYTES) == RADIO_AIRTIME_US(RADIO_UPLINK_BYTES));

    /* Wire layouts. A device parses these by offset; a padded struct would put
     * the hub's compiler in the protocol. */
    CHECK(sizeof(radio_data_beacon_t) == 14);
    CHECK(sizeof(radio_join_beacon_t) == 14);
    CHECK(sizeof(radio_pair_req_t)    == RADIO_PAIR_REQ_BYTES);

    /* Every frame has to load into one FIFO write, in both directions - the
     * length byte included. This is what forced compressed points on the wire,
     * ADR-0018, and PAIR_RSP is now the largest at 59 + 1. Checking only the
     * request would have left the frame that actually approaches the limit
     * unchecked, while reading like a check on frame size. */
    CHECK(RADIO_PAIR_REQ_BYTES    + 1u <= 66u);
    CHECK(RADIO_PAIR_RSP_BYTES    + 1u <= 66u);
    CHECK(RADIO_PAIR_CONF_BYTES   + 1u <= 66u);
    CHECK(RADIO_PAIR_ACCEPT_BYTES + 1u <= 66u);
    CHECK(RADIO_UPLINK_BYTES      + 1u <= 66u);

    /* Regions tile the superframe in order and leave the end guard alone. */
    CHECK(RADIO_BEACON_OFFSET_US == 0u);
    CHECK(RADIO_DOWNLINK_OFFSET_US == RADIO_BEACON_OFFSET_US + RADIO_BEACON_LEN_US);
    CHECK(RADIO_UPLINK_OFFSET_US == RADIO_DOWNLINK_OFFSET_US + RADIO_DOWNLINK_LEN_US);
    CHECK(RADIO_JOIN_OFFSET_US == RADIO_UPLINK_OFFSET_US + RADIO_UPLINK_LEN_US);
    CHECK(RADIO_JOIN_OFFSET_US + RADIO_JOIN_LEN_US + RADIO_END_GUARD_US <= SUPERFRAME_US);

    /* The beacon must fit the region that is named after it. */
    CHECK(BEACON_AIR < RADIO_BEACON_LEN_US);
    CHECK(DOWNLINK_AIR < RADIO_DOWNLINK_LEN_US);
    CHECK(JOIN_AIR + RADIO_JOIN_RX_US <= RADIO_JOIN_LEN_US);

    /* Slot arithmetic: slot 0 opens the uplink region, the last one closes it,
     * and a slot holds the frame it was sized for. */
    CHECK(RADIO_SLOT_OFFSET_US(0) == RADIO_UPLINK_OFFSET_US);
    CHECK(RADIO_SLOT_OFFSET_US(RADIO_SLOT_COUNT - 1u) + RADIO_SLOT_US
          == RADIO_JOIN_OFFSET_US);
    CHECK(RADIO_UPLINK_AIR_US < RADIO_SLOT_US);
    CHECK(RADIO_SLOT_US - RADIO_UPLINK_AIR_US == RADIO_SLOT_GUARD_US);

    /* The grid has to hold the 64 devices the protocol is specified for, with
     * something left over - a slot count exactly at the target leaves nothing
     * for a device that needs two. */
    CHECK(RADIO_SLOT_COUNT >= 64u);

    /* Duty cycle. Both directions matter: the sustainable combination must fit,
     * and the combination the code deliberately avoids must not - otherwise the
     * half-rate downlink and join beacon are cargo cult rather than a budget. */
    CHECK(BEACON_AIR + JOIN_AIR / 2u + DOWNLINK_AIR / RADIO_DOWNLINK_EVERY
          <= RADIO_AIR_BUDGET_US);
    CHECK(BEACON_AIR + JOIN_AIR + DOWNLINK_AIR > RADIO_AIR_BUDGET_US);
    CHECK(RADIO_AIR_BUDGET_US == SUPERFRAME_US / 100u);

    /* The exchange deliberately breaks the steady-state air budget, and both
     * directions are asserted: that one frame exceeds a superframe's whole
     * allowance, and that the exchange still fits comfortably inside the
     * quiesce it runs in. Checking only the second would let someone "fix" the
     * first by shrinking a frame and quietly lose the reason it is allowed. */
    CHECK(RADIO_AIRTIME_US(RADIO_PAIR_RSP_BYTES) > RADIO_AIR_BUDGET_US);

    /* Three denominators, all three asserted, because the exception is only
     * deliberate if the number it exceeds is named rather than chosen.
     *
     * Across the superframes the hub is actually parked on the join channel it
     * is over the per-superframe design budget - 41.92 ms in 4 s, 1.048%. That
     * budget is this project's own rule, not the regulation: ETSI's observation
     * period is an hour, over which one pairing is 0.0012%. Averaged over the
     * whole quiesce it is 0.52% and inside.
     *
     * Asserting only the comfortable denominator would let the exception be
     * defended by arithmetic instead of by reasoning. */
    CHECK(RADIO_PAIR_HUB_AIR_US >
          RADIO_PAIR_CLEAR_FRAMES * RADIO_AIR_BUDGET_US);
    CHECK(RADIO_PAIR_HUB_AIR_US <
          RADIO_QUIESCE_SUPERFRAMES * RADIO_AIR_BUDGET_US);

    /* The device's half is its own transmitter's budget and comfortably fits
     * the clear superframes, which is why only the hub's needs an exception. */
    CHECK(RADIO_PAIR_DEV_AIR_US <
          RADIO_PAIR_CLEAR_FRAMES * RADIO_AIR_BUDGET_US);

    /* The exchange has to fit the air it is given at all - a frame budget that
     * fits the duty cycle but not the wall clock is no budget. */
    CHECK(RADIO_PAIR_HUB_AIR_US + RADIO_PAIR_DEV_AIR_US <
          RADIO_PAIR_CLEAR_FRAMES * SUPERFRAME_US / 4u);

    /* The steady-state frame is the opposite case and gets no exception: it has
     * to fit the slot it was sized for, with the guard band left intact. */
    CHECK(RADIO_AIRTIME_US(RADIO_UPLINK_BYTES) <= RADIO_UPLINK_AIR_US);
    CHECK(RADIO_AIRTIME_US(RADIO_UPLINK_BYTES) + RADIO_SLOT_GUARD_US
          <= RADIO_SLOT_US);

    /* A device honouring the granted rate must stay under the same 1% every
     * other transmitter does. Asserted at the granted default rather than at
     * the geometry, because the geometry allows a rate the budget does not. */
    CHECK(RADIO_AIRTIME_US(RADIO_UPLINK_BYTES)
          <= RADIO_REPORT_EVERY_DEFAULT * RADIO_AIR_BUDGET_US);
    /* And the other direction: every superframe is *not* comfortable, which is
     * why the rate is granted rather than compiled in. A future change that
     * makes the frame small enough to report every superframe should have to
     * notice this line rather than silently pass. */
    CHECK(RADIO_UPLINK_AIR_US < RADIO_AIR_BUDGET_US);
    CHECK(RADIO_UPLINK_AIR_US * 10u > RADIO_AIR_BUDGET_US * 8u);

    /* A quiesce is announced in one byte and clamped by both ends to the same
     * value, so it has to be representable in one. */
    CHECK(RADIO_QUIESCE_SUPERFRAMES >= 1u && RADIO_QUIESCE_SUPERFRAMES <= 255u);

    /* The announce run is spent beaconing on the hop channels, so a quiesce
     * that is all announcement leaves the exchange no clear air at all. */
    CHECK(RADIO_QUIESCE_ANNOUNCE >= 2u);
    CHECK(RADIO_QUIESCE_SUPERFRAMES > RADIO_QUIESCE_ANNOUNCE);

    /* The rate limit is what bounds a *sequence* of announcements, which the
     * per-beacon clamp cannot see. At a gap shorter than the quiesce itself,
     * quiesce takes more than half the air time and the bound is gone. */
    CHECK(RADIO_QUIESCE_MIN_GAP >= RADIO_QUIESCE_SUPERFRAMES);

    /* The clamp has to be loose enough that two disciplined clocks never trip it
     * and tight enough that one lost beacon - a 2x period sample - cannot pass. */
    CHECK(SUPERFRAME_PERIOD_TOL_PCT >= 1u);
    CHECK(SUPERFRAME_PERIOD_TOL_PCT < 50u);

    /* One day, and a whole number of superframes: a rotation epoch that did not
     * divide evenly would put the two ends on different sides of a boundary. */
    CHECK(SUPERFRAME_PER_DAY == 43200u);
    CHECK(86400ull * 1000000ull % SUPERFRAME_US == 0u);

    printf("slots: %s (%u slots, %u us each, guard %u us, hub idle %.3f%%)\n",
           fails ? "FAIL" : "ok",
           RADIO_SLOT_COUNT, RADIO_SLOT_US, RADIO_SLOT_GUARD_US,
           100.0 * BEACON_AIR / SUPERFRAME_US);
    return fails ? 1 : 0;
}
