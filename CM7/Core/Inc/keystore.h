#pragma once

#include <stdint.h>

/* Persistent record of every device the operator has enrolled.
 *
 * This is the hub half of pairing's out-of-band step: the operator supplies a
 * device identifier and that device's public key, and the key exchange later
 * refuses any device whose key does not match. Without it, ECDH is anonymous
 * and trivially relayed - see docs/security/key-lifecycle.md.
 *
 * The operator used to supply a *fingerprint*. ADR-0021 changed it to the key
 * itself, because pair_v3 needs Z1 = X(hub_static * dev_static) before any
 * frame exists and a hash cannot be turned back into a curve point. The
 * out-of-band property is unchanged: a public key carried over the operator's
 * channel is self-authenticating and strictly more informative than its hash.
 *
 * The fingerprint is still what `device list` shows, computed from the stored
 * key rather than stored beside it, so the two can never disagree. */

#define KS_FINGERPRINT_BYTES  32u   /* SHA-256 of the point; displayed, not stored */
#define KS_PUBKEY_BYTES       33u   /* compressed SEC1 - ADR-0018 */
#define KS_ROOT_KEY_BYTES     32u
#define KS_MAX_DEVICES        64u

enum {
    KS_TYPE_DEVICE = 1,
    KS_TYPE_HUBKEY = 2,     /* the hub's own long-term private key */
    /* The network hop key. Every device gets the same 16 bytes, delivered in
     * PAIR_ACCEPT sealed under its own session key. It cannot come from the
     * pairing secret: that secret is pairwise, and a pairwise hop key gives
     * every device a different permutation while the hub has one radio and
     * sends one beacon per superframe. */
    KS_TYPE_NETKEY = 3
};

enum {
    KS_STATE_FREE = 0,
    KS_STATE_ENROLLED,   /* operator supplied a public key; not yet paired */
    KS_STATE_PAIRED,     /* key exchange completed, root key present */
    KS_STATE_DELETED     /* tombstone: the log is append-only */
};

/* 128 bytes: four H7 flash words exactly. See keystore.c. */
typedef struct ks_record {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  state;
    uint8_t  slot;                              /* uplink slot, assigned at enrol */
    uint32_t seq;                               /* newest record for a dev_id wins */
    uint32_t dev_id;
    uint32_t key_gen;                           /* generation rx_floor belongs to */
    /* The rotation epoch this root key was established at, where an epoch is
     * `superframe / SUPERFRAME_PER_DAY`. The daily ratchet key is
     * ratchet(root_key, now_epoch - rotate_epoch), so the current key is always
     * derivable from what is stored plus the shared counter - and rotation
     * costs no flash writes at all, only HKDF steps.
     *
     * It has to be stored because the ratchet is not stateless: K(n) cannot be
     * computed without knowing which n the root corresponds to. Deriving it
     * from wall time is not available - there is no RTC on either core, and a
     * clock that resets to 1970 on a power cut would silently re-derive keys
     * that were already used. */
    uint32_t rotate_epoch;
    uint32_t rx_floor;                          /* scoped to key_gen, never to a device */
    uint32_t tx_floor;                          /* carried forward across pairings */
    /* The device's static public key, compressed. One byte longer than the
     * fingerprint it replaced, paid for out of spare[] so the record stays 128
     * bytes and the sector layout does not move. */
    uint8_t  pubkey[KS_PUBKEY_BYTES];
    uint8_t  root_key[KS_ROOT_KEY_BYTES];       /* HKDF root; zero until pairing lands */
    /* The generation the exchange produced, kept rather than re-derived: after
     * a reboot the radio core needs it to open the next uplink frame, and
     * re-deriving it would mean keeping Z, which is 64 bytes of raw ECDH
     * output this record has no room for and no reason to hold. */
    uint8_t  session_key[16];
    /* The last dev_nonce accepted from this device. A repeat is refused, which
     * catches a stuck RNG in every mode - not only the all-zero one, which is
     * the unseeded signature rather than the common silicon failure - and
     * refuses a replayed PAIR_REQ as a side effect. Raised by the device side,
     * whose own RNG can reach PAIR_REQ having never produced a word since
     * power-on, because its identity is loaded from flash rather than drawn. */
    uint8_t  last_nonce[8];
    /* Deliberate, not accidental: the record is padded to a whole number of
     * flash words, and the padding is named so the next field has somewhere to
     * go without another format migration. */
    uint8_t  spare[3];
    uint32_t crc;
} ks_record_t;

/* Must run before the scheduler starts: recovery may erase a 128 KB sector,
 * which stalls the bank CM7 executes from for up to 1.4 s. */
uint8_t ks_init(void);

/* Enrols a device, or replaces the public key of one already enrolled. Assigns
 * the lowest free slot. Returns 0 on success, and writes the slot to slot_out. */
int ks_enrol(uint32_t dev_id, const uint8_t pubkey[KS_PUBKEY_BYTES],
             uint8_t *slot_out);

/* Tombstones a device. Its slot becomes free. Returns 0 if it was there. */
int ks_forget(uint32_t dev_id);

/* Completes a pairing: stores the derived material and moves the record to
 * KS_STATE_PAIRED. `rotate_epoch` is superframe / SUPERFRAME_PER_DAY at the
 * moment the key was agreed - stored now because the ratchet is not stateless
 * and cannot be told which generation a root belongs to after the fact.
 *
 * The ratchet state itself is NOT stored, because the agreed base case is
 * R(0) = Z and Z is 64 bytes this record has no room for. Rotation grows the
 * record when rotation is built; storing a 32-byte root for it now would mean
 * storing a value from the scheme that was not chosen. Returns 0 on success. */
int ks_pair_complete(uint32_t dev_id, const uint8_t session_key[16],
                     const uint8_t dev_nonce[8], uint32_t rotate_epoch);

/* The network hop key, created on first use. Every device gets these same 16
 * bytes. Returns 0 if the key was copied out.
 *
 * There is no rotation path: PAIR_ACCEPT is the only sealed downlink that
 * exists, so changing this key means re-pairing every device. That is a chosen
 * property today, not an oversight - see docs/radio/pairing.md. */
int ks_net_key_get(uint8_t key[16]);

/* The hub's long-term private key. 0 if one is stored and was copied out. */
int ks_hub_key_get(uint8_t priv[32]);

/* Stores a long-term private key. **Refuses if one already exists**: replacing
 * it orphans every device ever paired, since their stored hub_static no longer
 * matches anything the hub can prove. Returns 0 on success, -2 if one is
 * already stored. */
int ks_hub_key_set(const uint8_t priv[32]);

uint32_t ks_count(void);
const ks_record_t *ks_at(uint32_t index);
const ks_record_t *ks_find(uint32_t dev_id);

/* Test scaffolding. Writes a record short by its final flash word - byte for
 * byte what a write torn at that point leaves behind - carrying a HIGHER seq
 * than any valid record. The high seq is the point: a torn record with a low
 * one would be ignored for the wrong reason and prove nothing. Returns 0 if the
 * partial record was written. */
int ks_write_torn(void);

uint32_t ks_writes(void);
uint32_t ks_errors(void);
uint32_t ks_last_flash_error(void);
/* Slots holding our magic with a different version: skipped, not reclaimed. */
uint32_t ks_stale_format(void);

/* Recovery of key records written before a format change. Read, verify against
 * a witness outside this store, and only then commit - the CRC validates the
 * record, not the shim's reading of it. */
int      ks_legacy_hub_key_get(uint8_t priv[KS_ROOT_KEY_BYTES]);
int      ks_legacy_pending(void);
int      ks_legacy_commit(void);
uint32_t ks_migrated(void);
uint32_t ks_slots_left(void);
