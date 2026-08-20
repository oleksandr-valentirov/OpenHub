#pragma once

#include <stdint.h>

/* Must match CM4's hub_id: it is bound into the salt and the transcript, so a
 * disagreement is a confirmation mismatch with nothing to diagnose. */
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
    uint32_t derive_failed;
    uint32_t bad_confirm;
    uint32_t no_pending;
    uint32_t store_failed;
    uint32_t timed_out;
    uint32_t installed;
    uint32_t install_failed;
    uint32_t errors;
    /* The fingerprint computed from the key that arrived, and the head of the
     * key itself. A refusal that names the check still leaves the operator
     * unable to see whether the domain or the key is what differs. */
    uint8_t  last_fp[32];
    uint8_t  last_pubkey[8];
} pairing_stats_t;

void PairingTask(void *argument);

const pairing_stats_t *pairing_get_stats(void);

/* 1 if the hub's public key has been recovered and was copied out. */
uint8_t pairing_hub_pubkey(uint8_t pub[33]);

/* What PAIR_ACCEPT grants. Settable so bench work can ask for every superframe
 * without every device in the field doing the same. */
uint8_t pairing_report_every(void);
void    pairing_set_report_every(uint8_t n);

/* Arms pair_v3's invitation for an open window. Called from the CLI, which
 * cannot do the work itself: deriving Z1 is a P-256 scalar multiplication and
 * cliTask's stack is nowhere near the 12 KB it needs. So this only records the
 * request and PairingTask does the arithmetic on its own stack. */
void    pairing_arm_init(uint32_t dev_id, uint32_t window_ms);
void    pairing_disarm_init(void);

typedef struct pairing_init_stats {
    uint32_t z1_derivations;   /* must be 1 per window, not 1 per frame */
    uint32_t built;
    uint32_t pushed;
    uint32_t push_failed;
    uint32_t derive_failed;
    uint32_t last_superframe;
    /* The last frame the *live* path built, kept so it can be inspected
     * without a second builder. A separate "build for printing" path is a
     * parallel copy of the thing under test - which is how the device side's
     * Z1 defect survived: its check reached the live verifier but stepped over
     * the one function only the live path runs. */
    uint8_t  last_frame[28];
    uint8_t  last_len;
    uint8_t  armed;
} pairing_init_stats_t;
const pairing_init_stats_t *pairing_init_get_stats(void);

/* The rotation epoch a key agreed now belongs to. Indexed by the superframe
 * counter and not by wall time: there is no RTC, and a clock resetting on a
 * power cut would silently re-derive keys that were already used. */
uint32_t pairing_epoch_now(void);
