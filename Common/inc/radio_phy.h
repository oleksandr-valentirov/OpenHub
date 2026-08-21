#pragma once

#include <stdint.h>

#include "radio_slots.h"

/* The physical layer as a hub/device contract, every value read back off a chip.
 * radio_devices_docs/radio/phy.md */

/* Sections are separated by how far each value can be trusted.
 * radio_devices_docs/radio/phy.md */

/* --- CONFIGURED: the channel grid ------------------------------------- */

/* 865.1 MHz upwards in 100 kHz steps; RegFrf reads 0xD8A000 for slot 14. */
#define RADIO_CH_BASE_HZ        865100000u
#define RADIO_CH_SPACING_HZ     100000u
#define RADIO_GRID_COUNT        29u

/* Reserved out of the hop set, so a keyless device has somewhere fixed to look. */
#define RADIO_JOIN_SLOT         14u
#define RADIO_HOP_COUNT         (RADIO_GRID_COUNT - 1u)

#define RADIO_SLOT_HZ(n)        (RADIO_CH_BASE_HZ + \
                                 ((uint32_t)(n) % RADIO_GRID_COUNT) * RADIO_CH_SPACING_HZ)
#define RADIO_JOIN_HZ           RADIO_SLOT_HZ(RADIO_JOIN_SLOT)

/* hop index -> grid slot, stepping over the reserved join channel. */
#define RADIO_HOP_TO_GRID(i)    ((uint32_t)(i) < RADIO_JOIN_SLOT ? (uint32_t)(i) \
                                                                : (uint32_t)(i) + 1u)

/* --- CONFIGURED: modulation ------------------------------------------- */

/* RADIO_BITRATE_BPS is in radio_slots.h, where the slot geometry derives from it.
 * radio_devices_docs/radio/phy.md */
#define RADIO_DEVIATION_HZ      25000u
/* Two filters, named apart: one assert cannot cover two widths.
 * radio_devices_docs/radio/phy.md */
#define RADIO_RX_BW_HUB_HZ      125000u
#define RADIO_RX_BW_DEV_HZ      117300u
#define RADIO_RX_BANDWIDTH_HZ   RADIO_RX_BW_HUB_HZ

/* An allowance, and one sample of four has already exceeded it. ROADMAP item 12.
 * radio_devices_docs/radio/phy.md */
#define RADIO_CARRIER_ERR_HZ    12000u
#define RADIO_SHAPING_BT_X10    5u         /* Gaussian, BT 0.5 */

/* --- CONFIGURED: framing ---------------------------------------------- */

/* Framing registers, both parts: radio_devices_docs/radio/phy.md */
#define RADIO_PREAMBLE_BYTES    4u
#define RADIO_SYNC_BYTES        4u
#define RADIO_SYNC_WORD         { 0x68u, 0x65u, 0x6Cu, 0x6Cu }   /* "hell" */
#define RADIO_SYNC_TOLERANCE    0u         /* bit errors allowed in the sync word */

/* Variable length: a leading length byte, no filtering, no DC-free coding.
 * radio_devices_docs/radio/phy.md */
#define RADIO_LENGTH_BYTES      1u
#define RADIO_DCFREE_NONE       1
#define RADIO_ADDR_FILTER_NONE  1

/* The ceiling on any frame: the SX1231 FIFO is 66 bytes and holds the length byte.
 * radio_devices_docs/radio/phy.md */
#define RADIO_MAX_PAYLOAD_B     (66u - RADIO_LENGTH_BYTES)

/* CRC-16-CCITT, two bytes, final value inverted - the device selects
 * CRC_2_BYTE_INV. radio_devices_docs/radio/phy.md */
#define RADIO_CRC_BYTES         2u
#define RADIO_CRC_POLY          0x1021u    /* dev regs 0x06BE/0x06BF */
#define RADIO_CRC_SEED          0x1D0Fu    /* dev regs 0x06BC/0x06BD */
#define RADIO_CRC_INVERTED      1

/* --- CONFIGURED: power ------------------------------------------------ */

/* RegPaLevel 0x5F: PA1, +13 dBm, and not the reset default PA0.
 * radio_devices_docs/radio/phy.md */
#define RADIO_HUB_TX_DBM        13
#define RADIO_DEV_TX_DBM        14

/* --- DERIVED: air time ------------------------------------------------ */

/* Everything the transmitter keys for, not just the payload. */
#define RADIO_PHY_OVERHEAD_B    (RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES + \
                                 RADIO_LENGTH_BYTES + RADIO_CRC_BYTES)
#define RADIO_FRAME_AIR_US(payload_b) \
    (((uint32_t)(payload_b) + RADIO_PHY_OVERHEAD_B) * RADIO_US_PER_BYTE)

/* Air still to come when SyncAddressMatch fires, moving a stamp to frame end.
 * radio_devices_docs/radio/phy.md */
#define RADIO_PRE_SYNC_AIR_US   ((RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES) * \
                                 RADIO_US_PER_BYTE)
#define RADIO_POST_SYNC_AIR_US(payload_b) \
    (RADIO_FRAME_AIR_US(payload_b) - RADIO_PRE_SYNC_AIR_US)

/* --- CONTRACT: when each side may transmit and must listen ------------- */

/* Contract, not implementation: a device answering outside these is not heard.
 * radio_devices_docs/radio/phy.md */
#define RADIO_UPLINK_RX_OPEN_US   RADIO_UPLINK_OFFSET_US    /*   50 000 */
#define RADIO_UPLINK_RX_CLOSE_US  RADIO_JOIN_OFFSET_US      /* 1 874 000 */

/* Starts when the join beacon ends; RADIO_JOIN_RX_US is in radio_slots.h. */

/* When the hub transmits, not when a receiver should start.
 * radio_devices_docs/radio/phy.md */
#define RADIO_DOWNLINK_RX_OPEN_US   RADIO_DOWNLINK_OFFSET_US    /* 25 000 */
#define RADIO_DOWNLINK_RX_CLOSE_US  RADIO_UPLINK_OFFSET_US      /* 50 000 */
#define RADIO_DOWNLINK_ON(superframe) \
    (((uint32_t)(superframe) % RADIO_DOWNLINK_EVERY) == 0u)

/* Lead time is not guard band, and it applies to receive as well as transmit.
 * radio_devices_docs/radio/tdma.md */

/* --- MEASURED: this bench, this hardware ------------------------------ */

/* Measured, and not to be compiled against; the table is in
 * radio_devices_docs/radio/phy.md */

/* --- UNMEASURED: named so it is not mistaken for the above ------------- */

/* Three things nothing has measured.
 * radio_devices_docs/radio/phy.md */

_Static_assert(RADIO_JOIN_HZ == 866500000u, "join channel moved");
_Static_assert(RADIO_JOIN_SLOT < RADIO_GRID_COUNT, "join slot is off the grid");
_Static_assert(RADIO_HOP_COUNT == RADIO_GRID_COUNT - 1u,
               "the hop set is the grid minus the reserved join channel");
_Static_assert(RADIO_SLOT_HZ(RADIO_GRID_COUNT - 1u) <= 868000000u,
               "the grid runs past the sub-band");

/* Carson plus carrier error, and strictly wider: equality is no margin at all.
 * radio_devices_docs/radio/phy.md */
#define RADIO_RX_BW_MIN_HZ      (2u * (RADIO_DEVIATION_HZ + RADIO_BITRATE_BPS / 2u + \
                                       RADIO_CARRIER_ERR_HZ))

_Static_assert(RADIO_RX_BW_HUB_HZ > RADIO_RX_BW_MIN_HZ,
               "hub RX bandwidth leaves no room for the carrier to be off centre");

/* The device's filter is not asserted here yet: it is 117300 and would fail.
 * ROADMAP item 23, radio_devices_docs/radio/phy.md */

/* Modulation index 2, which the demodulator settings assume.
 * radio_devices_docs/radio/phy.md */
_Static_assert(2u * RADIO_DEVIATION_HZ >= RADIO_BITRATE_BPS, "modulation index below 1");

/* radio_slots.h's literal against the fields it is the sum of. */
_Static_assert(RADIO_FRAME_OVERHEAD_B == RADIO_PHY_OVERHEAD_B,
               "the slot grid was sized for a different frame overhead");

/* The two headers must agree about air time, or one describes another radio. */
_Static_assert(RADIO_FRAME_AIR_US(RADIO_UPLINK_BYTES) <= RADIO_SLOT_US,
               "an uplink frame does not fit its slot");

/* The halves must sum to the frame, or the stamp moves to the wrong place.
 * radio_devices_docs/radio/phy.md */
_Static_assert(RADIO_PRE_SYNC_AIR_US + RADIO_POST_SYNC_AIR_US(RADIO_UPLINK_BYTES)
               == RADIO_FRAME_AIR_US(RADIO_UPLINK_BYTES),
               "the sync edge does not split the frame's air time");
_Static_assert(RADIO_POST_SYNC_AIR_US(RADIO_UPLINK_BYTES) == 6720u,
               "post-sync air moved; every event-latency figure shifts with it");
_Static_assert(RADIO_UPLINK_RX_CLOSE_US > RADIO_UPLINK_RX_OPEN_US,
               "the uplink receive window is empty");

/* The region must hold the frame with guard, and must not reach uplink slot 0.
 * radio_devices_docs/radio/phy.md */
_Static_assert(RADIO_DOWNLINK_RX_CLOSE_US > RADIO_DOWNLINK_RX_OPEN_US,
               "the downlink receive window is empty");
_Static_assert(RADIO_DOWNLINK_RX_CLOSE_US <= RADIO_UPLINK_RX_OPEN_US,
               "the downlink region overruns the first uplink slot");
_Static_assert(RADIO_FRAME_AIR_US(RADIO_DOWNLINK_BYTES) < RADIO_DOWNLINK_LEN_US,
               "a downlink frame does not fit its region");
