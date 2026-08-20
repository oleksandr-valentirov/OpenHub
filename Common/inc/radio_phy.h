#pragma once

#include <stdint.h>

#include "radio_slots.h"

/* The physical layer, as a hub/device contract.
 *
 * Every value here was read back off a chip, not copied out of a driver call -
 * see the register column. `rfm69_set_power_dbm` returned success for months
 * while selecting a power amplifier that is not bonded to the antenna, so a
 * call that succeeded is not evidence that a field holds what was asked for.
 *
 * These constants lived in CM4/Core/Src/radio.c, where the device side could
 * not see them and had to define its own. An assert tying two definitions the
 * same side owns pins nothing about a contract: PAIR_FRAME_LEN was 45 on the
 * device and 49 here, both asserted, both internally consistent, and pairing
 * would have been refused on length with no diagnosable cause. Anything both
 * radios must agree on belongs in this file.
 *
 * Sections are separated by how much they can be trusted:
 *   CONFIGURED  written to a register and read back
 *   DERIVED     computed from the above; asserted below
 *   MEASURED    observed on hardware, will move if the hardware does
 *   UNMEASURED  named so it is not mistaken for one of the others
 */

/* --- CONFIGURED: the channel grid ------------------------------------- */

/* 865.1 MHz upwards in 100 kHz steps. RegFrf on the hub reads 0xD8A000 for
 * slot 14, which is 866 500 000 Hz exactly - the assert below pins the whole
 * arithmetic to that one verified point. */
#define RADIO_CH_BASE_HZ        865100000u
#define RADIO_CH_SPACING_HZ     100000u
#define RADIO_GRID_COUNT        29u

/* Reserved out of the hop set, so a joining device with no key has somewhere
 * fixed to look and the two can never collide. */
#define RADIO_JOIN_SLOT         14u
#define RADIO_HOP_COUNT         (RADIO_GRID_COUNT - 1u)

#define RADIO_SLOT_HZ(n)        (RADIO_CH_BASE_HZ + \
                                 ((uint32_t)(n) % RADIO_GRID_COUNT) * RADIO_CH_SPACING_HZ)
#define RADIO_JOIN_HZ           RADIO_SLOT_HZ(RADIO_JOIN_SLOT)

/* hop index -> grid slot, stepping over the reserved join channel. Both sides
 * must map identically or the sequences diverge from the first cycle. */
#define RADIO_HOP_TO_GRID(i)    ((uint32_t)(i) < RADIO_JOIN_SLOT ? (uint32_t)(i) \
                                                                : (uint32_t)(i) + 1u)

/* --- CONFIGURED: modulation ------------------------------------------- */

/* RADIO_BITRATE_BPS is in radio_slots.h, because the slot geometry is derived
 * from it and a second definition here could drift from the one the grid uses.
 *
 * hub  RegBitrate 0x0500, RegFdev 0x0199, RegDataModul 0x02, RegRxBw 0x8A
 * dev  SetModulationParams, RX bandwidth 117.3 kHz - the nearest SX126x step
 *      above the hub's 100 kHz. The two need not match; each receiver's
 *      bandwidth only has to cover the transmitted signal.
 */
#define RADIO_DEVIATION_HZ      25000u
#define RADIO_RX_BANDWIDTH_HZ   100000u    /* hub; the device uses 117300 */
#define RADIO_SHAPING_BT_X10    5u         /* Gaussian, BT 0.5 */

/* --- CONFIGURED: framing ---------------------------------------------- */

/* hub  RegPreamble 0x0004, RegSyncConfig 0x98, RegSyncValue1..4,
 *      RegPacketConfig1 0x98, RegPacketConfig2 0x02
 * dev  regs 0x06C0..0x06C3 for the sync word
 */
#define RADIO_PREAMBLE_BYTES    4u
#define RADIO_SYNC_BYTES        4u
#define RADIO_SYNC_WORD         { 0x68u, 0x65u, 0x6Cu, 0x6Cu }   /* "hell" */
#define RADIO_SYNC_TOLERANCE    0u         /* bit errors allowed in the sync word */

/* Variable length: a leading length byte, no address filtering, no DC-free
 * coding. Manchester was removed for doubling air time; the two parts also use
 * different whitening LFSRs, so whitening is not available either. */
#define RADIO_LENGTH_BYTES      1u
#define RADIO_DCFREE_NONE       1
#define RADIO_ADDR_FILTER_NONE  1

/* The ceiling on any frame, and the reason it is stated rather than implied:
 * the SX1231 FIFO is 66 bytes and in variable-length mode the length byte sits
 * in it, so the payload cannot exceed 65. A frame over it is not truncated on
 * air - the transmitter refuses to build it, which reads as a silent radio.
 *
 * The device part has a larger buffer, so this limit belongs to the hub alone
 * and the device cannot discover it by trying. It constrains every future
 * frame: an ephemeral P-256 point is 33 bytes and a full-length MAC is 16,
 * leaving 16 for everything else in a frame carrying both. pair_v3 is designed
 * against this number. */
#define RADIO_MAX_PAYLOAD_B     (66u - RADIO_LENGTH_BYTES)

/* CRC-16-CCITT, two bytes, **final value inverted**.
 *
 * This is the field that cost four pairing windows, and it is not derivable
 * from the polynomial and the seed: the SX126x's CRC_2_BYTE and CRC_2_BYTE_INV
 * share both and differ only in that inversion. The SX1231 computes the
 * inverted form and has no setting for the other, so the device must select
 * CRC_2_BYTE_INV explicitly. Chosen wrong, beacons arrive and fail CRC and the
 * band looks empty from both ends.
 *
 * Established by the device side's controlled sweep: 2inv decodes, 2 fails CRC
 * on the same beacons, off ignores the checksum entirely.
 */
#define RADIO_CRC_BYTES         2u
#define RADIO_CRC_POLY          0x1021u    /* dev regs 0x06BE/0x06BF */
#define RADIO_CRC_SEED          0x1D0Fu    /* dev regs 0x06BC/0x06BD */
#define RADIO_CRC_INVERTED      1

/* --- CONFIGURED: power ------------------------------------------------ */

/* RegPaLevel 0x5F: PA1, +13 dBm. **Not PA0** - the pin is not bonded to the
 * antenna on an RFM69HW/HCW, and the reset default selects it. The hub ran
 * that way for months at a measured -40 dBm EIRP with every call returning
 * success. See .claude/skills/rfm69/SKILL.md. */
#define RADIO_HUB_TX_DBM        13
#define RADIO_DEV_TX_DBM        14

/* --- DERIVED: air time ------------------------------------------------ */

/* Everything the transmitter keys for, not just the payload. The preamble is
 * settable and the CRC is two bytes or none, so a caller computing this from
 * the payload length alone is correct only until someone changes either.
 *
 * radio_slots.h carries RADIO_FRAME_OVERHEAD_B as the literal 11, because the
 * slot geometry needs it before this header exists. The assert at the bottom
 * ties that literal to the fields it is the sum of, so changing the preamble
 * breaks the build rather than silently resizing every slot. */
#define RADIO_PHY_OVERHEAD_B    (RADIO_PREAMBLE_BYTES + RADIO_SYNC_BYTES + \
                                 RADIO_LENGTH_BYTES + RADIO_CRC_BYTES)
#define RADIO_FRAME_AIR_US(payload_b) \
    (((uint32_t)(payload_b) + RADIO_PHY_OVERHEAD_B) * RADIO_US_PER_BYTE)

/* --- CONTRACT: when each side may transmit and must listen ------------- */

/* These live only in code today and cost four windows to establish. They are
 * contract, not implementation: a device that answers outside them is not
 * heard, and the silence names nothing.
 *
 * The hub's uplink receiver is open across the whole slot region rather than
 * per slot - it has one receiver and no reason to retune 96 times.
 *
 * **That 91% is not a tolerance, and the sentence here used to say it was.**
 * It said a slot-timing disagreement could be ruled out by arithmetic because
 * the receiver is open for 91% of the superframe. The 91% is true and it is
 * the wrong quantity: it bounds how much of the superframe the receiver is
 * listening, not how far a slot may be out. The real margins are
 *
 *   slot 0    0 us early   - the region starts exactly where the downlink ends
 *   slot 95   1400 us late - RADIO_SLOT_US minus RADIO_UPLINK_AIR_US
 *
 * so the interior slots have most of the region either side and the two ends
 * have the slot guard and nothing more. The device on this bench uses slot 0,
 * where the early margin is zero.
 *
 * And "the hub still hears it" is not the failure that matters. Once there is
 * more than one device, a slot-offset disagreement above 1400 us puts adjacent
 * devices on top of each other - and a receiver open across the whole region
 * makes that *harder* to see, because both frames land inside it. Slot timing
 * has to be measured. It cannot be ruled out from this number.
 *
 * Found by re-reading this file for bounds justified by citing a property,
 * after the device side lost an evening to a comment that cited the hop
 * generator's proven "every channel once per cycle" to bound the *gap* between
 * two uses of one channel. The citation was true; the inference was not.
 */
#define RADIO_UPLINK_RX_OPEN_US   RADIO_UPLINK_OFFSET_US    /*   50 000 */
#define RADIO_UPLINK_RX_CLOSE_US  RADIO_JOIN_OFFSET_US      /* 1 874 000 */

/* The join-channel receive starts when the join beacon ends and the request's
 * own air time comes out of it. RADIO_JOIN_RX_US is in radio_slots.h. */

/* The downlink, verified end to end on 2026-08-20: three frames on three hop
 * channels, tags opened by an implementation sharing no code with this one.
 *
 * A device has to be listening at this offset or it hears nothing, so the
 * window is contract exactly as the uplink one turned out to be - and it is
 * written down here *before* either side builds to it, rather than being
 * recovered from the other's source after a window is spent on it.
 *
 * Which superframes carry one is the other half of the contract and is easy to
 * get wrong by one: the hub transmits when `superframe % RADIO_DOWNLINK_EVERY
 * == 0`, so a device listening on the odd ones hears an empty region forever
 * and nothing anywhere is in error. Half rate is a duty-cycle decision, not a
 * convenience: beacon + downlink + join beacon every superframe is 28.5 ms,
 * 1.42%, over the ETSI limit.
 *
 * The device stays in receive for the whole region rather than for the frame's
 * air time. The hub's own beacon jitter, its lead time and the device's clock
 * error all land inside the region, and a receiver opened for 12 ms of air in a
 * 25 ms region can miss a frame that was transmitted correctly.
 *
 * **These bounds are when the hub transmits, not when a receiver should start.**
 * Receive entry is lead time in exactly the sense below: the part is still
 * ramping while the preamble goes past, so a receiver opened *at* the offset
 * misses a frame that was sent correctly. Measured on the device side, whose
 * receiver opened at +25 000 and heard nothing for eight superframes while the
 * hub's frame sat at +25 059 - every counter on both sides individually
 * correct, and an empty region indistinguishable from a region aimed wrongly.
 * The device now opens 8 ms early, which is clear of the beacon.
 *
 * The general form: a window's start is a transmit schedule. Each receiver
 * subtracts its own entry time from it, and that time is a property of the part
 * rather than of this contract. */
#define RADIO_DOWNLINK_RX_OPEN_US   RADIO_DOWNLINK_OFFSET_US    /* 25 000 */
#define RADIO_DOWNLINK_RX_CLOSE_US  RADIO_UPLINK_OFFSET_US      /* 50 000 */
#define RADIO_DOWNLINK_ON(superframe) \
    (((uint32_t)(superframe) % RADIO_DOWNLINK_EVERY) == 0u)

/* Lead time is not guard band. A transmitter's oscillator warm-up is known in
 * advance and scheduled around by keying early; drift is uncertainty and needs
 * guard on both sides. Keeping them apart is why the guard did not move when
 * the device's warm-up fell from 10.4 ms to 2.4 ms.
 *
 * Each side measures its own lead as `keyed - air time` per frame rather than
 * trusting a constant. The device's is about 2.9 ms; the hub's differs.
 *
 * It applies to receive as well as transmit, which is the half that was missed:
 * see the downlink window above. Both directions of both radios have a warm-up,
 * and only the transmit ones had ever been written down. */

/* --- MEASURED: this bench, this hardware ------------------------------ */

/* Not constants to compile against - they move when the hardware does, and are
 * here so a future measurement has something to disagree with.
 *
 *   hub HSE            +4091 to +4703 ppm   ST-Link MCO, X3 unfitted
 *   superframe         2 010 944 us         device's measurement of the hub
 *   device TCXO ramp   2871 to 2891 us      keyed minus air time, per frame
 *   first bit in slot  +244 to +245 us      against a 1400 us guard
 *   beacon -> request  25 084 +/- 20 us
 *   link, 1 m          -23 to -27 dBm       both directions
 *   hub noise floor    -94 to -108 dBm      depends on the channel
 */

/* --- UNMEASURED: named so it is not mistaken for the above ------------- */

/* The **receiver's** required preamble. RADIO_PREAMBLE_BYTES is what each side
 * transmits, and 4 bytes is known to work into both receivers. What either bit
 * synchroniser actually needs has never been established - a sweep at 4 and 16
 * bytes came back green at both, because the fault that prompted it was
 * elsewhere. Do not read "4 works" as "4 is sufficient with margin".
 *
 * The **top of RADIO_MAX_PAYLOAD_B**. 65 is arithmetic on a datasheet FIFO
 * size, not an observation: the largest frame either side has ever transmitted
 * is 59 bytes. A frame designed at exactly 65 would fly at a boundary nothing
 * has tested, so pair_v3 leaves margin rather than spending it.
 *
 * The **slot byte's position** in the uplink header and in the GCM nonce. Both
 * vectors and both implementations use slot 0, so every byte of that field has
 * been zero in every frame ever exchanged. It gets its first real test when a
 * second device is granted slot 1.
 */

_Static_assert(RADIO_JOIN_HZ == 866500000u, "join channel moved");
_Static_assert(RADIO_JOIN_SLOT < RADIO_GRID_COUNT, "join slot is off the grid");
_Static_assert(RADIO_HOP_COUNT == RADIO_GRID_COUNT - 1u,
               "the hop set is the grid minus the reserved join channel");
_Static_assert(RADIO_SLOT_HZ(RADIO_GRID_COUNT - 1u) <= 868000000u,
               "the grid runs past the sub-band");

/* Carson's rule. A receive bandwidth narrower than this does not fail loudly -
 * it clips the sidebands and the symptom is a link that works at short range
 * and not at long, which reads as a power problem. */
_Static_assert(RADIO_RX_BANDWIDTH_HZ >= 2u * (RADIO_DEVIATION_HZ + RADIO_BITRATE_BPS / 2u),
               "RX bandwidth is under Carson's rule for this deviation");

/* Modulation index 2. The demodulator settings assume a wide-index signal;
 * halving the deviation without revisiting them changes sensitivity silently. */
_Static_assert(2u * RADIO_DEVIATION_HZ >= RADIO_BITRATE_BPS, "modulation index below 1");

/* The literal in radio_slots.h against the fields it is the sum of. This is the
 * one assert here that pins two definitions to each other rather than pinning a
 * value to itself: the slot grid was sized with 11 bytes of overhead, and the
 * preamble, sync word, length byte and CRC are what make it 11. */
_Static_assert(RADIO_FRAME_OVERHEAD_B == RADIO_PHY_OVERHEAD_B,
               "the slot grid was sized for a different frame overhead");

/* The two headers must agree about air time. radio_slots.h sized the slot from
 * a payload and this file sizes a frame from the same byte rate; if they ever
 * disagree, one of them is describing a different radio. */
_Static_assert(RADIO_FRAME_AIR_US(RADIO_UPLINK_BYTES) <= RADIO_SLOT_US,
               "an uplink frame does not fit its slot");
_Static_assert(RADIO_UPLINK_RX_CLOSE_US > RADIO_UPLINK_RX_OPEN_US,
               "the uplink receive window is empty");

/* The downlink region has to hold a downlink frame with the guard the beacon's
 * jitter and the device's clock error need, and it must not run into the first
 * uplink slot - a hub still transmitting when slot 0 opens collides with the
 * device it is talking to. */
_Static_assert(RADIO_DOWNLINK_RX_CLOSE_US > RADIO_DOWNLINK_RX_OPEN_US,
               "the downlink receive window is empty");
_Static_assert(RADIO_DOWNLINK_RX_CLOSE_US <= RADIO_UPLINK_RX_OPEN_US,
               "the downlink region overruns the first uplink slot");
_Static_assert(RADIO_FRAME_AIR_US(28u) < RADIO_DOWNLINK_LEN_US,
               "a downlink frame does not fit its region");
