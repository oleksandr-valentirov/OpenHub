#pragma once

#include <stdint.h>
#include "ipc.h"

/**
 * @file pairing.h
 * @brief The hub's half of the four-frame exchange, and the counters it is read by.
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */

/** @brief Must match CM4's hub_id: it is bound into the salt and the transcript. */
#define PAIRING_HUB_ID  0x33442211u

typedef struct pairing_stats {
    uint32_t reqs;
    uint32_t derived;
    uint32_t confs;
    uint32_t paired;
    uint32_t bad_len;
    uint32_t not_enrolled;
    uint32_t bad_fingerprint;
    uint32_t zero_nonce;
    uint32_t repeat_nonce;
    uint32_t no_hub_key;
    uint32_t no_eph_key;     /**< the ephemeral keygen refused, before any schedule */
    uint32_t derive_failed;
    uint32_t bad_confirm;
    uint32_t no_pending;
    uint32_t store_failed;
    uint32_t timed_out;
    uint32_t installed;
    uint32_t install_failed;
    uint32_t errors;
    uint8_t  last_fp[32];    /**< of the key that arrived, not of the enrolled one */
    uint8_t  last_pubkey[8]; /**< ... and its head, so domain and key are separable */
} pairing_stats_t;

/**
 * @brief The pairing task: does every scalar multiplication, on its own 12 KB stack.
 * @param argument  unused, required by the CMSIS-RTOS signature
 */
void PairingTask(void *argument);

/**
 * @brief The exchange's counters, refusal reasons included.
 * @return the live block; every refusal has its own field rather than one total
 */
const pairing_stats_t *pairing_get_stats(void);

/**
 * @brief The bytes the last confirmations were actually taken over.
 * @param dev_id      receives the device the transcript belongs to
 * @param superframe  receives the transcript's own superframe, not the live counter
 * @return the 119 bytes, or NULL when no exchange has completed
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
const uint8_t *pairing_last_transcript(uint32_t *dev_id, uint32_t *superframe);

/**
 * @brief The reporting cadence PAIR_ACCEPT grants.
 * @return superframes between reports, granted at pairing rather than compiled in
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
uint8_t pairing_report_every(void);

/**
 * @brief Uplink arrivals CM4 pushed, so nothing infers a cadence from a poll.
 * @param short_payload  receives the count CM7 judged too short
 * @param last_tick      receives when the last one was handled
 * @param last           receives the last report itself
 * @return arrivals since boot
 *
 * A derived denominator is an assumption with a column heading, so this counts
 * what arrived and never what should have. ROADMAP item 2
 */
uint32_t pairing_uplink_events(uint32_t *short_payload, uint32_t *last_tick,
                               ipc_device_report_t *last);

/**
 * @brief Doorbell interrupts, read against the poll timeouts that stand in for them.
 * @param timeouts  receives the polls that fired because no doorbell did
 * @return doorbells taken since boot
 *
 * The poll fallback works, which is exactly why a dead doorbell is otherwise silent.
 */
uint32_t pairing_doorbells(uint32_t *timeouts);
/**
 * @brief Sets the cadence the next pairing is granted.
 * @param n  superframes between reports; devices already in the field keep theirs
 */
void    pairing_set_report_every(uint8_t n);

/**
 * @brief Arms pair_v4's invitation for one device.
 * @param dev_id     the enrolled device to invite
 * @param window_ms  how long the window stays open
 *
 * Records the request only: deriving Z1 is a scalar multiplication and cliTask's
 * stack is nowhere near what it needs. radio_devices_docs/open_hub/radio/pairing.md
 */
void    pairing_arm_init(uint32_t dev_id, uint32_t window_ms);

/** @brief Closes the invitation window early. */
void    pairing_disarm_init(void);

typedef struct pairing_init_stats {
    uint32_t z1_derivations;   /**< must be 1 per window, not 1 per frame */
    uint32_t built;
    uint32_t pushed;
    uint32_t push_failed;
    uint32_t derive_failed;
    uint32_t last_superframe;
    uint8_t  last_frame[61]; /**< what the live path built, never a second builder */
    uint8_t  last_len;
    uint8_t  armed;
} pairing_init_stats_t;
/**
 * @brief The invitation path's counters.
 * @return the live block, carrying the frame the live path built rather than a copy
 */
const pairing_init_stats_t *pairing_init_get_stats(void);

/**
 * @brief The rotation epoch a key agreed now belongs to.
 * @return superframe / SUPERFRAME_PER_DAY, indexed by the counter rather than stepped
 *
 * radio_devices_docs/open_hub/radio/pairing.md
 */
uint32_t pairing_epoch_now(void);
