#pragma once

#include <stdint.h>

/* The slot grid inside one superframe, in nominal microseconds and never ticks.
 * radio_devices_docs/radio/tdma.md */

/* 50 kbps buys the slots k=3 needs; deviation stays 25 kHz, so h falls to 1.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_BITRATE_BPS       50000u
#define RADIO_US_PER_BYTE       (8u * 1000000u / RADIO_BITRATE_BPS)   /* 160 */

/* Keyed for but never payload; the duty cycle counts these too.
 * radio_devices_docs/radio/phy.md */
#define RADIO_PREAMBLE_BYTES    4u
#define RADIO_SYNC_BYTES        4u
#define RADIO_LENGTH_BYTES      1u
#define RADIO_CRC_BYTES         2u
#define RADIO_FRAME_OVERHEAD_B  (RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES + \
                                 RADIO_LENGTH_BYTES + RADIO_CRC_BYTES)

/* Intervals, named at both ends, because the origin is what call sites got wrong.
 * radio_devices_docs/radio/phy.md */
#define RADIO_AIR_START_TO_END_US(payload_b) \
    (((uint32_t)(payload_b) + RADIO_FRAME_OVERHEAD_B) * RADIO_US_PER_BYTE)
#define RADIO_AIR_START_TO_SYNC_US \
    ((RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES) * RADIO_US_PER_BYTE)
#define RADIO_AIR_SYNC_TO_END_US(payload_b) \
    (RADIO_AIR_START_TO_END_US(payload_b) - RADIO_AIR_START_TO_SYNC_US)

/* Contract constant, not a hint: a device must clamp its estimate to TOL_PCT.
 * radio_devices_docs/radio/tdma.md */
#define SUPERFRAME_US           2000000u
#define SUPERFRAME_PERIOD_TOL_PCT  1u

/* One day of superframes, from the counter and never from wall time.
 * radio_devices_docs/radio/crypto/key-lifecycle.md */

/* 64-bit intermediate: a day in microseconds overflows 32 bits. */
#define SUPERFRAME_PER_DAY  ((uint32_t)(86400ull * 1000000ull / SUPERFRAME_US))

/* Elapsed superframes come from the beacon's field, never from beacons counted.
 * radio_devices_docs/radio/tdma.md */

/* The largest counter jump a device accepts from a beacon.
 * radio_devices_docs/radio/joining.md */
#define RADIO_RESYNC_MAX_JUMP   SUPERFRAME_PER_DAY

/* A sealed uplink frame at its ceiling: 7 B header, 16 B body, 16 B GCM tag.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_UPLINK_PAYLOAD_B  39u
#define RADIO_UPLINK_AIR_US     RADIO_AIR_START_TO_END_US(RADIO_UPLINK_PAYLOAD_B)  /* 8000 */

/* Guard is uncertainty on both sides of the frame, and never lead time.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_SLOT_GUARD_US     1400u
#define RADIO_SLOT_US           (RADIO_UPLINK_AIR_US + RADIO_SLOT_GUARD_US)  /* 9400 */

/* Regions from the superframe boundary. A LEN is the region, not the frame's
 * air time. radio_devices_docs/radio/tdma.md */
#define RADIO_BEACON_OFFSET_US  0u
#define RADIO_BEACON_LEN_US     25000u

/* One hub-to-device frame, at most every second superframe.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_DOWNLINK_OFFSET_US  (RADIO_BEACON_OFFSET_US + RADIO_BEACON_LEN_US)
#define RADIO_DOWNLINK_LEN_US     25000u
#define RADIO_DOWNLINK_EVERY      2u

#define RADIO_UPLINK_OFFSET_US  (RADIO_DOWNLINK_OFFSET_US + RADIO_DOWNLINK_LEN_US)
#define RADIO_SLOT_COUNT        194u
#define RADIO_UPLINK_LEN_US     (RADIO_SLOT_COUNT * RADIO_SLOT_US)

/* Slot N opens here, and assignment counts up from 0. */
#define RADIO_SLOT_OFFSET_US(n) (RADIO_UPLINK_OFFSET_US + (uint32_t)(n) * RADIO_SLOT_US)

/* Three opportunities a superframe, because two bottom out at exactly 1000 ms.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_SLOT_OPPS         3u

/* Device d transmits in d, d + STRIDE, d + 2*STRIDE; doubling inverts by modulo.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_SLOT_STRIDE       65u
#define RADIO_SLOT_NTH_US(d, k) RADIO_SLOT_OFFSET_US((uint32_t)(d) + \
                                    (uint32_t)(k) * RADIO_SLOT_STRIDE)
#define RADIO_SLOT_TO_DEVICE(n) ((uint32_t)(n) % RADIO_SLOT_STRIDE)

/* The device cap falls out of the geometry rather than out of a grant. */
#define RADIO_DEVICE_MAX        (RADIO_SLOT_COUNT - \
                                 (RADIO_SLOT_OPPS - 1u) * RADIO_SLOT_STRIDE)

/* The gap that wraps the boundary, which is the largest of the three. */
#define RADIO_EVENT_GAP_US      (SUPERFRAME_US - (RADIO_SLOT_OPPS - 1u) * \
                                 RADIO_SLOT_STRIDE * RADIO_SLOT_US)

/* An event at a sensor, delivered and handled here, no later than this.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_EVENT_DEADLINE_US 1000000u

/* The device's warm figure; cold from sleep is unmeasured and larger.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_UPLINK_LATCH_US   3250u

/* What the deadline leaves for CM4 receive to CM7 handled, which is unmeasured. */
#define RADIO_HUB_HANDLE_SLACK_US (RADIO_EVENT_DEADLINE_US - RADIO_EVENT_GAP_US - \
                                   RADIO_UPLINK_LATCH_US - RADIO_UPLINK_AIR_US)

/* Occupied only while a pairing window is open, overlapping the uplink tail.
 * radio_devices_docs/open_hub/radio/superloop.md */
#define RADIO_JOIN_OFFSET_US    (RADIO_UPLINK_OFFSET_US + RADIO_UPLINK_LEN_US)
#define RADIO_JOIN_RX_US        100000u
#define RADIO_JOIN_LEN_US       116000u

/* Never scheduled. The boundary must be reachable without a frame in flight. */
#define RADIO_END_GUARD_US      10000u

/* 1% of the superframe, the ETSI allowance, and the hub's own transmissions only.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_AIR_BUDGET_US     (SUPERFRAME_US / 100u)

/* The pairing exchange is outside this budget, deliberately.
 * radio_devices_docs/radio/tdma.md */

/* One device at `opps` opportunities a superframe, in ppm, derived.
 * radio_devices_docs/radio/phy.md */
#define RADIO_DUTY_PPM(opps)    ((uint32_t)((uint64_t)(opps) * RADIO_UPLINK_AIR_US \
                                            * 1000000u / SUPERFRAME_US))

/* ERC REC 70-03 for 865-868 MHz, per transmitter.
 * radio_devices_docs/radio/phy.md */
#define RADIO_DUTY_LIMIT_PPM    10000u

/* k = 3 is 12000 ppm, over; k = 2 is the fallback that must stay legal.
 * radio_devices_docs/radio/phy.md */
_Static_assert(RADIO_DUTY_PPM(2u) <= RADIO_DUTY_LIMIT_PPM,
               "two opportunities a superframe exceed the band's duty cycle");
_Static_assert(RADIO_DUTY_PPM(RADIO_SLOT_OPPS) > RADIO_DUTY_LIMIT_PPM,
               "k = 3 now fits the duty cycle - ROADMAP item 10 can close");

/* Every frame the exchange puts on air, in bytes.
 * radio_devices_docs/radio/tdma.md */

/* pair_v4's invitation, which unlike the rest is recurring air. ADR-0021 */

/* Every 4th superframe: 0.156%, less than the join beacon it replaces. ADR-0021 */

/* How long an operator's window stays open, for both cores.
 * radio_devices_docs/open_hub/radio/pairing.md */
#define RADIO_PAIR_WINDOW_MS    60000u
#define RADIO_PAIR_INIT_EVERY   4u
/* The device must not answer before the hub has turned its radio around.
 * radio_devices_docs/radio/pairing.md */
#define RADIO_PAIR_REQ_LEAD_US  30000u

#define RADIO_PAIR_INIT_BYTES   61u
#define RADIO_PAIR_REQ_BYTES    56u
#define RADIO_PAIR_RSP_BYTES    58u
#define RADIO_PAIR_CONF_BYTES   26u
#define RADIO_PAIR_ACCEPT_BYTES 50u

/* The steady-state frame a paired device sends in its own slot. */
#define RADIO_UPLINK_BYTES      39u
#define RADIO_DOWNLINK_BYTES    39u

/* Split by transmitter: a budget belongs to a radio, not to a conversation.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_PAIR_HUB_AIR_US   (RADIO_AIR_START_TO_END_US(RADIO_PAIR_RSP_BYTES) + \
                                 RADIO_AIR_START_TO_END_US(RADIO_PAIR_ACCEPT_BYTES))
#define RADIO_PAIR_DEV_AIR_US   (RADIO_AIR_START_TO_END_US(RADIO_PAIR_REQ_BYTES) + \
                                 RADIO_AIR_START_TO_END_US(RADIO_PAIR_CONF_BYTES))

/* The superframes the hub is actually parked on the join channel.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_PAIR_CLEAR_FRAMES (RADIO_QUIESCE_SUPERFRAMES - RADIO_QUIESCE_ANNOUNCE)

/* Handed out in PAIR_ACCEPT: every 8th is 0.11%, every one is 0.88%.
 * radio_devices_docs/open_hub/radio/pairing.md */
#define RADIO_REPORT_EVERY_DEFAULT  8u

/* How long the grid may be suspended, and both ends clamp to it.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_QUIESCE_SUPERFRAMES  4u

/* How many superframes carry the announcement, which is not free time.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_QUIESCE_ANNOUNCE     2u

/* Normal traffic required between one quiesce and the next.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_QUIESCE_MIN_GAP      4u

/* A cost assertion, not a behavioural one: normal is 1-10 us and a fault is
 * milliseconds. radio_devices_docs/radio/tdma.md */
#ifndef RADIO_BEACON_LATE_LIMIT_US       /* lowered to prove the detector
                                          * non-vacuous */
#define RADIO_BEACON_LATE_LIMIT_US 500u
#endif

/* The geometry, asserted where it is defined rather than in one test.
 * radio_devices_docs/radio/tdma.md */

/* Device d's three slots must not be another device's. */
_Static_assert(RADIO_DEVICE_MAX <= RADIO_SLOT_STRIDE, "device sets overlap");
_Static_assert((RADIO_SLOT_OPPS - 1u) * RADIO_SLOT_STRIDE + RADIO_DEVICE_MAX <=
               RADIO_SLOT_COUNT, "the last device's last slot is off the end");

/* Holds only while both sides use all three slots. radio_devices_docs/radio/tdma.md */
_Static_assert(RADIO_EVENT_GAP_US + RADIO_UPLINK_LATCH_US + RADIO_UPLINK_AIR_US <
               RADIO_EVENT_DEADLINE_US, "the widest gap misses the event deadline");

/* Where the target lives: below 64 the grid stops meeting the brief. */
_Static_assert(RADIO_DEVICE_MAX >= 64u, "fewer than 64 devices fit");

/* radio_uplink_t.slot is one byte, so the grid cannot outgrow it. */
_Static_assert(RADIO_SLOT_COUNT <= 256u, "the slot field cannot address the grid");

/* Every region, in order, inside one superframe. */
_Static_assert(RADIO_JOIN_OFFSET_US + RADIO_JOIN_LEN_US + RADIO_END_GUARD_US <=
               SUPERFRAME_US, "the regions overrun the superframe");
