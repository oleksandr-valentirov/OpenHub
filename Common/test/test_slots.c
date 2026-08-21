/**
 * @file test_slots.c
 * @brief Not that the geometry is good, but that it is still consistent with itself.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */

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
#define BEACON_AIR   RADIO_AIR_START_TO_END_US(sizeof(radio_data_beacon_t))
#define JOIN_AIR     RADIO_AIR_START_TO_END_US(sizeof(radio_join_beacon_t))
#define DOWNLINK_AIR RADIO_AIR_START_TO_END_US(sizeof(radio_downlink_t))

int main(void) {
    /* The join channel from the grid arithmetic, against the read-back RegFrf. */
    CHECK(RADIO_JOIN_HZ == 866500000u);
    CHECK(RADIO_SLOT_HZ(0) == RADIO_CH_BASE_HZ);
    /* The mapping must step over the reserved slot, not through it. */
    CHECK(RADIO_HOP_TO_GRID(RADIO_JOIN_SLOT - 1u) == RADIO_JOIN_SLOT - 1u);
    CHECK(RADIO_HOP_TO_GRID(RADIO_JOIN_SLOT) == RADIO_JOIN_SLOT + 1u);
    CHECK(RADIO_HOP_TO_GRID(RADIO_HOP_COUNT - 1u) == RADIO_GRID_COUNT - 1u);
    /* Two names for one quantity; folded, so there is nothing left to compare. */

    /* A padded struct would put the hub's compiler in the protocol. */
    CHECK(sizeof(radio_data_beacon_t) == 14);
    CHECK(sizeof(radio_join_beacon_t) == 14);
    CHECK(sizeof(radio_pair_req_t)    == RADIO_PAIR_REQ_BYTES);

    /* Every frame, both directions, length byte included. ADR-0018
     * radio_devices_docs/open_hub/testing/host-tests.md */
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

    /* Slot 0 opens the region, the last closes it, and each holds its frame. */
    CHECK(RADIO_SLOT_OFFSET_US(0) == RADIO_UPLINK_OFFSET_US);
    CHECK(RADIO_SLOT_OFFSET_US(RADIO_SLOT_COUNT - 1u) + RADIO_SLOT_US
          == RADIO_JOIN_OFFSET_US);
    CHECK(RADIO_UPLINK_AIR_US < RADIO_SLOT_US);
    CHECK(RADIO_SLOT_US - RADIO_UPLINK_AIR_US == RADIO_SLOT_GUARD_US);

    /* 64 devices, and at k=3 that is exact by construction rather than spare. */
    CHECK(RADIO_DEVICE_MAX >= 64u);
    CHECK(RADIO_DEVICE_MAX == RADIO_SLOT_COUNT -
                              (RADIO_SLOT_OPPS - 1u) * RADIO_SLOT_STRIDE);

    /* Three slots, one device, one modulo - and the ends of the range. */
    CHECK(RADIO_SLOT_TO_DEVICE(0u) == 0u);
    CHECK(RADIO_SLOT_TO_DEVICE(RADIO_SLOT_STRIDE) == 0u);
    CHECK(RADIO_SLOT_TO_DEVICE(2u * RADIO_SLOT_STRIDE) == 0u);
    CHECK(RADIO_SLOT_TO_DEVICE(RADIO_DEVICE_MAX - 1u) == RADIO_DEVICE_MAX - 1u);
    CHECK(RADIO_SLOT_TO_DEVICE(RADIO_SLOT_COUNT - 1u) == RADIO_DEVICE_MAX - 1u);

    /* The slots between the sets belong to no device, and must not be read as 0. */
    CHECK(RADIO_SLOT_TO_DEVICE(RADIO_DEVICE_MAX) >= RADIO_DEVICE_MAX);

    /* The widest gap is the one that wraps, and it has to leave the deadline room. */
    CHECK(RADIO_EVENT_GAP_US ==
          SUPERFRAME_US - (RADIO_SLOT_OPPS - 1u) * RADIO_SLOT_STRIDE * RADIO_SLOT_US);
    CHECK(RADIO_EVENT_GAP_US + RADIO_UPLINK_LATCH_US + RADIO_UPLINK_AIR_US
          < RADIO_EVENT_DEADLINE_US);

    /* Both directions: what must fit, and what must not.
     * radio_devices_docs/open_hub/testing/host-tests.md */
    CHECK(BEACON_AIR + JOIN_AIR / 2u + DOWNLINK_AIR / RADIO_DOWNLINK_EVERY
          <= RADIO_AIR_BUDGET_US);
    CHECK(RADIO_AIR_BUDGET_US == SUPERFRAME_US / 100u);

    /* 25 kbps forced half rate; at 50 kbps the three are 16.0 ms, 0.800%.
     * radio_devices_docs/radio/phy.md */
    CHECK(BEACON_AIR + JOIN_AIR + DOWNLINK_AIR <= RADIO_AIR_BUDGET_US);

    /* The binding negative is the device's now: k=3 every superframe is over 1%.
     * radio_devices_docs/radio/phy.md */
    CHECK(RADIO_UPLINK_AIR_US * RADIO_SLOT_OPPS > RADIO_AIR_BUDGET_US);
    CHECK(RADIO_UPLINK_AIR_US * (RADIO_SLOT_OPPS - 1u) <= RADIO_AIR_BUDGET_US);

    /* 22.4 ms at 25 kbps broke the budget; 11.2 ms fits it.
     * radio_devices_docs/radio/tdma.md */
    CHECK(RADIO_AIR_START_TO_END_US(RADIO_PAIR_RSP_BYTES) <= RADIO_AIR_BUDGET_US);

    /* Three denominators, all three asserted.
     * radio_devices_docs/open_hub/testing/host-tests.md */
    CHECK(RADIO_PAIR_HUB_AIR_US <=
          RADIO_PAIR_CLEAR_FRAMES * RADIO_AIR_BUDGET_US);
    CHECK(RADIO_PAIR_HUB_AIR_US <
          RADIO_QUIESCE_SUPERFRAMES * RADIO_AIR_BUDGET_US);

    /* The device's half is its own budget, and needs no exception. */
    CHECK(RADIO_PAIR_DEV_AIR_US <
          RADIO_PAIR_CLEAR_FRAMES * RADIO_AIR_BUDGET_US);

    /* The exchange has to fit the wall clock it is given, not only the duty. */
    CHECK(RADIO_PAIR_HUB_AIR_US + RADIO_PAIR_DEV_AIR_US <
          RADIO_PAIR_CLEAR_FRAMES * SUPERFRAME_US / 4u);

    /* The steady-state frame gets no exception: its slot, with guard intact. */
    CHECK(RADIO_AIR_START_TO_END_US(RADIO_UPLINK_BYTES) <= RADIO_UPLINK_AIR_US);
    CHECK(RADIO_AIR_START_TO_END_US(RADIO_UPLINK_BYTES) + RADIO_SLOT_GUARD_US
          <= RADIO_SLOT_US);

    /* At the granted default, not at the geometry, which allows more.
     * radio_devices_docs/open_hub/testing/host-tests.md */
    CHECK(RADIO_AIR_START_TO_END_US(RADIO_UPLINK_BYTES)
          <= RADIO_REPORT_EVERY_DEFAULT * RADIO_AIR_BUDGET_US);
    /* One frame is comfortable; the k=3 pair above is where it stops being.
     * radio_devices_docs/open_hub/testing/host-tests.md */
    CHECK(RADIO_UPLINK_AIR_US < RADIO_AIR_BUDGET_US);

    /* A quiesce is announced in one byte, so it must be representable in one. */
    CHECK(RADIO_QUIESCE_SUPERFRAMES >= 1u && RADIO_QUIESCE_SUPERFRAMES <= 255u);

    /* A quiesce that is all announcement leaves the exchange no clear air. */
    CHECK(RADIO_QUIESCE_ANNOUNCE >= 2u);
    CHECK(RADIO_QUIESCE_SUPERFRAMES > RADIO_QUIESCE_ANNOUNCE);

    /* The rate limit bounds a sequence, which the per-beacon clamp cannot see.
     * radio_devices_docs/radio/tdma.md */
    CHECK(RADIO_QUIESCE_MIN_GAP >= RADIO_QUIESCE_SUPERFRAMES);

    /* Loose enough for two disciplined clocks, tight enough to catch a 2x sample. */
    CHECK(SUPERFRAME_PERIOD_TOL_PCT >= 1u);
    CHECK(SUPERFRAME_PERIOD_TOL_PCT < 50u);

    /* One day, and a whole number of superframes.
     * radio_devices_docs/radio/crypto/key-lifecycle.md */
    CHECK(SUPERFRAME_PER_DAY == 43200u);
    CHECK(86400ull * 1000000ull % SUPERFRAME_US == 0u);

    printf("slots: %s (%u slots, %u us each, guard %u us, hub idle %.3f%%)\n",
           fails ? "FAIL" : "ok",
           RADIO_SLOT_COUNT, RADIO_SLOT_US, RADIO_SLOT_GUARD_US,
           100.0 * BEACON_AIR / SUPERFRAME_US);
    return fails ? 1 : 0;
}
