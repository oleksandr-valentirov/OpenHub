#pragma once

#include <stdint.h>

/* The slot grid inside one superframe.
 *
 * This is a hub/device contract, so it lives in Common rather than in either
 * core's private headers: the device computes its transmit moment from these
 * offsets and shares none of the hub's code.
 *
 * Every offset here is in **nominal microseconds**. The hub converts them
 * through timebase_us_to_ticks() because its own reference is not exact; a
 * device slaves to the beacon and needs no equivalent. Nothing in this file may
 * be expressed in timer ticks - a tick is a property of one board. */

#define RADIO_BITRATE_BPS       25000u
#define RADIO_US_PER_BYTE       (8u * 1000000u / RADIO_BITRATE_BPS)   /* 320 */

/* preamble 4 + sync 4 + length 1 + CRC 2, all sent by the modem around the
 * payload. Air time is what the duty cycle counts, so it is these bytes too. */
#define RADIO_FRAME_OVERHEAD_B  11u
#define RADIO_AIRTIME_US(payload_b) \
    (((uint32_t)(payload_b) + RADIO_FRAME_OVERHEAD_B) * RADIO_US_PER_BYTE)

/* The superframe period, and a **contract constant** rather than a hint.
 *
 * A device still measures the period from consecutive beacons - that costs no
 * air and does not require the hub to tell the truth - but it must clamp the
 * result to SUPERFRAME_PERIOD_TOL_PCT of this value and reject a sample outside
 * it rather than adopting it.
 *
 * Clamping makes the do-not-trust-the-hub property stronger, not weaker. An
 * unclamped estimator can be walked anywhere its acceptance window allows, by a
 * hostile hub or by a bench transmitter, and a device whose estimate has drifted
 * far enough sees every honest beacon as a backwards jump. Unclamped, the
 * estimator trusts the hub further than a constant would.
 *
 * The original reason for discovering the period rather than declaring it was
 * that the hub's reference ran 3750 ppm fast, so the nominal was not real time.
 * That reason is gone: the timebase is disciplined against LSE and the residual
 * is -27 ppm. See docs/radio/timebase.md.
 *
 * 1% is ~100x the worst case both clocks can produce together, so it never
 * bites an honest link - and it makes two specific poisoned samples impossible:
 * a period of 4 s, which is what one *lost* beacon looks like, and anything
 * short enough to race a device's counter ahead of the hub's. */
#define SUPERFRAME_US           2000000u
#define SUPERFRAME_PERIOD_TOL_PCT  1u

/* One day of superframes, and the unit the daily key ratchet is indexed by.
 *
 * Derived from the period rather than written as 43200, so the two cannot drift
 * apart. Both ends must agree on it: the rotation epoch is
 * `superframe / SUPERFRAME_PER_DAY`, and a hub and device that disagreed would
 * ratchet to different keys and simply stop hearing each other.
 *
 * Indexed by the counter, not by wall time. There is no RTC on either core, and
 * a wall clock that resets on a power cut would silently re-derive keys that
 * had already been used - the same nonce-reuse shape the durable counter
 * exists to prevent. The counter survives a reboot; a wall clock would not. */
/* 64-bit intermediate: a day in microseconds is 86.4e9 and overflows 32 bits,
 * which the host test caught by reading 0 instead of 43200. */
#define SUPERFRAME_PER_DAY  ((uint32_t)(86400ull * 1000000ull / SUPERFRAME_US))

/* A device must derive the number of elapsed superframes from the beacon's
 * `superframe` field, never from the number of beacons it heard. The hub
 * deliberately omits beacons during a quiesce, and any beacon can be lost, so
 * consecutive beacons are not consecutive superframes. */

/* A sealed uplink frame: 12 B header, 16 B payload, 16 B GCM tag. */
#define RADIO_UPLINK_PAYLOAD_B  44u
#define RADIO_UPLINK_AIR_US     RADIO_AIRTIME_US(RADIO_UPLINK_PAYLOAD_B)  /* 17600 */

/* Guard is uncertainty, on both sides of the frame: relative drift across one
 * superframe (~40 ppm between two LSE-class references, 80 us), preamble
 * detection and interrupt latency on a waking node, and slot quantisation.
 *
 * The device's oscillator start is deliberately NOT in here. It is lead time -
 * known in advance and scheduled around by waking early - and spending guard on
 * it would leave the guard oversized and the uncertainty still uncovered. That
 * is why this number did not move when the device's warm-up fell from 10.4 ms
 * to 2.4: the two budgets were never sharing. */
#define RADIO_SLOT_GUARD_US     1400u
#define RADIO_SLOT_US           (RADIO_UPLINK_AIR_US + RADIO_SLOT_GUARD_US)  /* 19000 */

/* Regions, as offsets from the superframe boundary. */
#define RADIO_BEACON_OFFSET_US  0u
#define RADIO_BEACON_LEN_US     25000u          /* 8.0 ms on air, rest is retune margin */

/* One hub-to-device frame, and at most every second superframe: see the air
 * budget below, which a downlink in every superframe does not fit inside. */
#define RADIO_DOWNLINK_OFFSET_US  (RADIO_BEACON_OFFSET_US + RADIO_BEACON_LEN_US)
#define RADIO_DOWNLINK_LEN_US     25000u
#define RADIO_DOWNLINK_EVERY      2u

#define RADIO_UPLINK_OFFSET_US  (RADIO_DOWNLINK_OFFSET_US + RADIO_DOWNLINK_LEN_US)
#define RADIO_SLOT_COUNT        96u
#define RADIO_UPLINK_LEN_US     (RADIO_SLOT_COUNT * RADIO_SLOT_US)

/* Slot N opens here. Assignment starts at 0 and counts up, which is what keeps
 * the join region below over unassigned slots rather than over live devices. */
#define RADIO_SLOT_OFFSET_US(n) (RADIO_UPLINK_OFFSET_US + (uint32_t)(n) * RADIO_SLOT_US)

/* Only occupied while a pairing window is open. It overlaps the tail of the
 * uplink region on purpose: a region reserved permanently would cost every
 * superframe for something that happens when a human presses a button. */
#define RADIO_JOIN_OFFSET_US    (RADIO_UPLINK_OFFSET_US + RADIO_UPLINK_LEN_US)
#define RADIO_JOIN_RX_US        100000u
#define RADIO_JOIN_LEN_US       116000u

/* Never scheduled. The boundary must be reachable without a frame in flight. */
#define RADIO_END_GUARD_US      10000u

/* 1% of the superframe, the ETSI allowance for 865-868 MHz. The hub's own
 * transmissions must fit inside this; a device's uplink is its own budget.
 *
 * Beacon 8.0 ms + downlink 12.5 ms + join beacon 8.0 ms is 28.5 ms and does not
 * fit, which is why the downlink runs at half rate and the join beacon does
 * too. There is no governor yet - the hub trusts the schedule - so any change
 * here has to be re-measured with tools/sdr/dutycycle.py. */
#define RADIO_AIR_BUDGET_US     (SUPERFRAME_US / 100u)

/* **The pairing exchange is outside this budget, deliberately.**
 *
 * PAIR_RSP is 59 bytes, which is 22.4 ms on air - more than the whole 20 ms a
 * steady-state superframe allows. That is not a defect: the exchange runs
 * during a quiesce, when the hub is parked on the join channel and transmitting
 * no beacons at all, so the superframe it lands in has none of its usual
 * traffic. Averaged across the quiesce the whole exchange is about 66 ms over
 * four superframes, which is 0.8%.
 *
 * Written down because a duty-cycle governor comparing a single frame against
 * RADIO_AIR_BUDGET_US would flag this as a violation and be wrong. The budget
 * is per steady-state superframe; the exchange is the one thing in the system
 * that cannot be reasoned about with it.
 *
 * Raised by the device side, whose 256-byte buffer gives it no opinion on the
 * frame sizes at all - so the observation came from the shared air budget
 * rather than from either radio's framing. */
/* Every frame the exchange puts on air, in bytes. radio_protocol.h owns the
 * layouts and asserts each struct against the number here, so a frame that
 * grows cannot leave this budget describing the old one. It did once: ACCEPT
 * was charged 28 bytes while the frame became 50. */
/* pair_v3's invitation. Retried across the window, so unlike every other frame
 * here it is recurring air and its size is a duty-cycle decision - see
 * ADR-0021 and the layout in radio_protocol.h. */
/* Every 4th superframe, 8 s apart, ~7 attempts in a 60 s window. Not a
 * convenience: at every 2nd it is 1.048% against the 1% budget, and at 28
 * bytes every 4th it costs 0.156% - less than the 0.200% join beacon it
 * replaces. See ADR-0021 for the table. */
/* How long an operator's window stays open. Both cores need it now: CM4 closes
 * the window and CM7 arms the invitation for the same span, and two literals
 * would drift into a hub inviting a device it has stopped listening for. */
#define RADIO_PAIR_WINDOW_MS    60000u
#define RADIO_PAIR_INIT_EVERY   4u
#define RADIO_PAIR_INIT_BYTES   28u
#define RADIO_PAIR_REQ_BYTES    57u
#define RADIO_PAIR_RSP_BYTES    59u
#define RADIO_PAIR_CONF_BYTES   26u
#define RADIO_PAIR_ACCEPT_BYTES 50u

/* The steady-state frame a paired device sends in its own slot. */
#define RADIO_UPLINK_BYTES      31u
#define RADIO_DOWNLINK_BYTES    31u

/* Split by transmitter, because a duty-cycle budget belongs to a radio and not
 * to a conversation. Summing all four frames charged the hub for the two the
 * device sends - a check doing real arithmetic on the wrong quantity, which
 * would have fired for a device-side change that is none of the hub's business.
 * Raised by the device side, which is where the overcount was visible. */
#define RADIO_PAIR_HUB_AIR_US   (RADIO_AIRTIME_US(RADIO_PAIR_RSP_BYTES) + \
                                 RADIO_AIRTIME_US(RADIO_PAIR_ACCEPT_BYTES))
#define RADIO_PAIR_DEV_AIR_US   (RADIO_AIRTIME_US(RADIO_PAIR_REQ_BYTES) + \
                                 RADIO_AIRTIME_US(RADIO_PAIR_CONF_BYTES))

/* The superframes the hub is actually parked on the join channel: the announce
 * run is spent beaconing on the hop channels, so the exchange does not have the
 * whole quiesce to spread itself over. Getting this denominator wrong is what
 * made 1.048% read as 0.91%. */
#define RADIO_PAIR_CLEAR_FRAMES (RADIO_QUIESCE_SUPERFRAMES - RADIO_QUIESCE_ANNOUNCE)

/* Handed out in PAIR_ACCEPT. A device reporting every superframe spends 17.6 ms
 * of slot per 2 s, which is 0.88% - under the limit but with nothing left for a
 * retry. Eight superframes is 0.11% and leaves the budget alone. The rate is
 * granted rather than compiled in, so bench work can ask for 1 without every
 * device in the field doing the same. */
#define RADIO_REPORT_EVERY_DEFAULT  8u

/* How long the grid may be suspended for a pairing exchange. The hub commits to
 * the resume superframe when it announces the quiesce and never extends it: 64
 * devices may be asleep against that number, so it is a promise, not a plan.
 *
 * A device clamps to the same value. Matching means a forged beacon cannot buy
 * an attacker more downtime than an honest one, which matters while the beacon
 * is still unauthenticated. */
#define RADIO_QUIESCE_SUPERFRAMES  4u

/* How many superframes carry the announcement. One is one lost frame away from
 * a device that wakes into its slot and transmits at a hub that is not
 * listening, so the announcement is repeated - with resume_in counting down, so
 * every copy names the same absolute superframe.
 *
 * The announce run is not free time: the hub is still beaconing on the hop
 * channels during it. RADIO_QUIESCE_SUPERFRAMES minus this is what the exchange
 * actually gets with the hub parked on the join channel. */
#define RADIO_QUIESCE_ANNOUNCE     2u

/* Superframes of normal traffic required between one quiesce ending and the
 * next being allowed to start.
 *
 * The one-byte clamp bounds a *single* announcement; it cannot see a rate. An
 * attacker replaying a well-formed announcement every fifth superframe holds a
 * device asleep indefinitely with every individual beacon inside spec. This
 * caps quiesce at half the air time, for the hub and the device alike - an
 * operator pairing several devices back to back pays for it in wall clock, and
 * a forger cannot exceed it either.
 *
 * Both ends enforce it. A hub that could exceed what devices accept would look
 * like an attacker to its own network. */
#define RADIO_QUIESCE_MIN_GAP      4u

/* A beacon later than this into its superframe means something in the superloop
 * became expensive. Not a tuned figure: normal is 1-10 us and anything that
 * could actually hurt - a flash erase, a blocking wait, a receive window left
 * open - is milliseconds. It only has to separate two numbers three orders
 * apart.
 *
 * This is a *cost* assertion rather than a behavioural one, and that is the
 * point. The beacon still goes out, the grid still steps, every counter still
 * agrees; the only thing that changes is how long it took. Nothing that checks
 * results can see that, which is exactly how the store's append-point bug hid
 * on the device side - a page erase per write, reported as success. */
#ifndef RADIO_BEACON_LATE_LIMIT_US       /* overridable so the detector can be
                                          * proved non-vacuous by lowering it
                                          * below the normal 1-10 us */
#define RADIO_BEACON_LATE_LIMIT_US 500u
#endif
