#pragma once

#include <stdint.h>
#include <stddef.h>

/* For the per-frame byte counts the slot budget is computed from. The sizes
 * belong to both files: this one owns the layout, that one owns the air time it
 * costs, and an assert below ties them together so the timing budget can never
 * be computed from a frame size that has moved. */
#include "radio_slots.h"
/* For RADIO_MAX_PAYLOAD_B. The hub's FIFO is the ceiling on every frame here
 * and the device cannot discover it by trying - its own buffer is larger. */
#include "radio_phy.h"


typedef struct protocol_header {
    uint32_t hub_id;
    uint32_t dev_id;
} __attribute__((packed)) protocol_header_t;

typedef struct radio_pairing {
    protocol_header_t header;
    uint8_t stage;
} __attribute__((packed)) protocol_pairing_t;

/* Frame types. The join beacon is the only one a device can read before it has
 * a key, so it carries nothing secret. */
enum {
    RADIO_FRAME_DATA_BEACON = 0x01,
    RADIO_FRAME_JOIN_BEACON = 0x02,
    RADIO_FRAME_PAIR_REQ    = 0x03,   /* device -> hub, join channel, cleartext */
    RADIO_FRAME_PAIR_RSP    = 0x04,   /* hub -> device, join channel, cleartext */
    RADIO_FRAME_PAIR_CONF   = 0x05,   /* device -> hub, join channel, cleartext */
    RADIO_FRAME_PAIR_ACCEPT = 0x06,   /* hub -> device, sealed: the slot grant */
    RADIO_FRAME_UPLINK      = 0x07,   /* device -> hub, sealed, in its own slot */
    RADIO_FRAME_DOWNLINK    = 0x08,   /* hub -> device, sealed, in the downlink region */
    RADIO_FRAME_PAIR_INIT   = 0x09    /* hub -> device, join channel, MACed (pair_v3) */
};

/* The version byte every frame in this header carries.
 *
 * It lived in CM4/Core/Src/radio.c, where the device side could not see it and
 * had picked 1 for the pairing frames while the beacons went out as 2. Two
 * self-consistent constants, two passing asserts, and a first pairing refused
 * on version with nothing to diagnose - the same defect as PAIR_FRAME_LEN 45
 * against 49, in a different field. It belongs here or nowhere. */
#define RADIO_PROTO_VERSION          2u

/* On the wire in the join beacon, the data beacon, PAIR_REQ and PAIR_INIT, so
 * both sides must agree - and it lived as a literal in CM4/Core/Src/radio.c,
 * where CM7 could not see it and the device kept its own copy. Exactly the
 * arrangement that let PAIR_FRAME_LEN be 45 on one side and 49 here. */
#define RADIO_NET_ID                 0x0001u

#define RADIO_JOIN_FLAG_WINDOW_OPEN  0x01

/* The hub is about to leave the hop channels to run a pairing exchange on the
 * join channel. Carried in the data beacon, which is the frame every paired
 * device already receives - a separate announcement frame would cost duty cycle
 * every superframe to say nothing almost every time. */
#define RADIO_BEACON_FLAG_QUIESCE    0x01

/* Sent every superframe on the hopping channel. This is where a paired device
 * takes its time alignment, so it belongs here rather than in a hub-private
 * header - a device has to decode it and cannot see CM4/Core/Inc/radio.h.
 *
 * It replaces a struct that began with a broadcast address byte and carried the
 * superframe counter in a field called "flags" and the hub's raw timer in one
 * called "clock". All three were reasonable while this was a hub-only debug
 * broadcast and became wrong the moment a device had to read it: the address
 * filtered nothing (RFM69_FILTER_NONE), the counter's name said nothing, and a
 * raw local timer means nothing off-board - it wraps every 71 minutes and
 * restarts at reset. */
/* The layout is frozen at 14 bytes. Nothing per-device goes here: a broadcast
 * that grows with the device count does not scale and costs duty cycle every
 * superframe. Slot assignment and grid geometry are delivered at pairing; the
 * superframe period is measured from consecutive beacons rather than advertised,
 * which needs no bytes and does not depend on the hub telling the truth.
 *
 * If it ever must grow, `version` changes with it. A receiver should check the
 * version first and the length second, so a future layout is rejected rather
 * than misparsed. */
/* v2 added flags and resume_in; v1 was the same struct without them. */
typedef struct radio_data_beacon {
    uint8_t  type;          /* RADIO_FRAME_DATA_BEACON, like every other frame */
    uint8_t  version;
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t superframe;    /* the protocol's clock, named for what it is */
    uint8_t  flags;
    /* Superframes from this beacon's `superframe` until normal traffic resumes,
     * so resume_at = superframe + resume_in. A countdown rather than a bare
     * flag because the announcement is the last frame a device will hear: told
     * only "quiescing", it must stay in RX to learn when that ended, which is
     * the opposite of what standing down is for. Told when, it sleeps.
     *
     * A device clamps this to RADIO_QUIESCE_SUPERFRAMES. The beacon is not
     * authenticated, so an unbounded value is either a bug or a forgery. */
    uint8_t  resume_in;     /* 0 when the grid is running */
} __attribute__((packed)) radio_data_beacon_t;

/* Sent on the fixed join channel while an operator has the pairing window open.
 * Carries the superframe counter so a joiner is already time-aligned by the
 * moment it has the key and can follow the hop sequence. */
typedef struct radio_join_beacon {
    uint8_t  type;
    uint8_t  version;
    uint16_t net_id;        /* public: it only has to identify the network */
    uint32_t hub_id;
    uint32_t superframe;
    uint8_t  flags;
    uint8_t  hop_channels;  /* size of the hop set, so the plan needs no guessing */
} __attribute__((packed)) radio_join_beacon_t;

/* Device -> hub, on the join channel, cleartext. The first frame of the pairing
 * exchange and the only one whose shape is settled: it is identity plus a
 * public key, and neither depends on what the exchange decides afterwards.
 *
 * `superframe` echoes the counter the device read from the join beacon. It
 * costs four bytes and proves alignment before the hub spends a scalar
 * multiplication on a device that is not actually on the grid.
 *
 * Replay of this frame is harmless because the hub's half of the exchange is
 * ephemeral: a replayed request derives a key the replayer cannot confirm.
 *
 * It is the only frame of the exchange that carries net_id: it is sent by a
 * device that has heard nothing but a join beacon, so it names the network it
 * believes it is joining. Every later frame is addressed by hub_id and dev_id.
 * See docs/radio/pairing.md. */
/* Asserted against the contract's literal sizes, not against sizeof itself.
 *
 * A receiver checks `len != sizeof(struct)`, so a struct that silently changed
 * size would move that check with it and reject every honest frame as the wrong
 * length - while an assert comparing the struct to itself passed happily.
 *
 * The literals live *here*, in the shared header both ends compile, and not in
 * either firmware. An assert against a length each side defines for itself is
 * the same vacuous check in a different costume: the device side had
 * PAIR_FRAME_LEN == 45 and this side had 49, both asserts passing, both
 * internally consistent, and pairing that would never have worked. */
_Static_assert(sizeof(radio_data_beacon_t) == 14, "data beacon is 14 bytes on the wire");
_Static_assert(sizeof(radio_join_beacon_t) == 14, "join beacon is 14 bytes on the wire");

/* pair_v3, ADR-0021. The hub invites a named device instead of waiting to be
 * found. Everything after this frame is pair_v2 unchanged.
 *
 * Retried across the operator's 60 s window, which is what fixes the size: it
 * is the one pairing frame that is recurring hub air, competing with the beacon
 * and the downlink inside the same 1%. An ephemeral point here would cost 61
 * bytes and be over budget at every retry rate fast enough to be useful, so the
 * ephemeral stays where pair_v2 already has it. At 28 bytes every 4th
 * superframe this costs 0.156%, less than the 0.200% join beacon it replaces.
 *
 * mac = HMAC-SHA256(K_init, the 16 bytes above it)[:12], where
 * K_init = HKDF(Z1, salt = hub_id||dev_id big-endian, "openhub/v3/init") and
 * Z1 = X(hub_static * dev_static). Both sides can compute Z1 before any frame
 * exists, which is what makes an addressed first frame safe to act on - and Z1
 * is already published as pair_v2's `pair_z1`, so this frame's key is a value
 * both implementations reproduce today.
 *
 * 96-bit truncation: the frame has headroom and a longer tag buys nothing
 * against a forger who gets one attempt per retry. **Freshness, not tag length,
 * is the weak point** - a device that needs pairing has no counter to check
 * `superframe` against, so a recorded PAIR_INIT replays until the device
 * persists a high-water mark. Device-side obligation, recorded in ADR-0021,
 * because the hub cannot observe whether it is honoured. */
/* The version byte this frame carries, here rather than as a literal each
 * firmware writes for itself. A size assert does not cover it: both sides can
 * agree on 28 bytes and disagree on byte 1, which is PAIR_FRAME_LEN being 45
 * on one side and 49 on the other with an assert passing on both.
 *
 * 3, while every other frame in the exchange carries RADIO_PROTO_VERSION = 2.
 * Deliberate: this frame exists only in pair_v3 and the frames after it are
 * pair_v2's, unchanged. Do not "fix" the inconsistency - the pinned vectors
 * carry 3 here and 2 there. Raised by the device side, which was checking the
 * byte against PV3_INIT_HEADER[1] at runtime because there was nothing shared
 * to compile against. */
#define RADIO_PAIR_INIT_VERSION  3u

typedef struct radio_pair_init {
    uint8_t  type;              /* RADIO_FRAME_PAIR_INIT */
    uint8_t  version;           /* RADIO_PAIR_INIT_VERSION */
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t dev_id;            /* addressed: only this device may answer */
    uint32_t superframe;
    uint8_t  mac[12];
} __attribute__((packed)) radio_pair_init_t;

_Static_assert(sizeof(radio_pair_init_t) == 28, "pair init is 28 bytes on the wire");
_Static_assert(RADIO_PAIR_INIT_VERSION != RADIO_PROTO_VERSION,
               "pair_v3's invitation is deliberately a different version byte "
               "from the pair_v2 frames that follow it");
/* The MAC covers everything before it, so this is the split rather than a sum
 * of field sizes - a sum can stay correct while the offset it describes moves. */
#define RADIO_PAIR_INIT_MAC_LEN  12u
_Static_assert(offsetof(radio_pair_init_t, mac) ==
               sizeof(radio_pair_init_t) - RADIO_PAIR_INIT_MAC_LEN,
               "the MAC must cover exactly the cleartext ahead of it");

typedef struct radio_pair_req {
    uint8_t  type;          /* RADIO_FRAME_PAIR_REQ */
    uint8_t  version;
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t dev_id;
    uint32_t superframe;
    /* The device's only contribution of freshness, and the exchange is
     * replayable without it.
     *
     * Z is a function of the two static keys and hub_eph alone, so a recorded
     * PAIR_RSP re-derives the same session key forever: replay it at the next
     * pairing and the device installs an old key with every check passing.
     * Binding the superframe helps and is not enough - an unpaired device takes
     * its counter from a join beacon that is cleartext by necessity, so a
     * forged beacon feeds it whatever superframe the recording used.
     *
     * A zero nonce is refused: it is what an RNG that was not ready leaves
     * behind, and it would restore exactly the property this field removes. */
    uint8_t  dev_nonce[8];
    uint8_t  pubkey[33];    /* compressed SEC1 point - ADR-0018 */
} __attribute__((packed)) radio_pair_req_t;

_Static_assert(sizeof(radio_pair_req_t) == 57, "pair request is 57 bytes on the wire");

/* The direction byte of the GCM nonce. wire-crypto.md says "direction (1)" and
 * stops, so the two values are pinned here rather than in either firmware -
 * they are exactly the kind of constant both sides define for themselves and
 * then disagree about, with a tag failure as the only symptom. */
/* Neither value is zero, deliberately. With uplink at 0x00 a nonce left unset
 * by a memset is a *correct* uplink nonce and a broken downlink one, so the bug
 * ships, passes the bench in the direction that was tested first, and surfaces
 * only in the other. Both directions failing immediately is worth more than
 * matching the conventional numbering. */
#define RADIO_DIR_UPLINK    0x01u   /* device -> hub */
#define RADIO_DIR_DOWNLINK  0x02u   /* hub -> device */

/* The slot field of the nonce for frames that belong to no uplink slot. Real
 * slots are 0..RADIO_SLOT_COUNT-1, so this range cannot collide with one.
 * The low byte is a retry index: a retransmission inside the same superframe
 * would otherwise repeat the nonce exactly. */
#define RADIO_NONCE_SLOT_UNSLOTTED  0xFFFF00u

/* Hub -> device, cleartext, in answer to a PAIR_REQ.
 *
 * No net_id, unlike PAIR_REQ, and the asymmetry is the point: the device
 * learned net_id from the join beacon before it transmitted, and hub_id already
 * says which hub is answering. What this frame must carry that the request need
 * not is dev_id - two devices can be joining in one window, and the second has
 * to ignore the first's response rather than derive a key from it. */
typedef struct radio_pair_rsp {
    uint8_t  type;              /* RADIO_FRAME_PAIR_RSP */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint8_t  eph_pubkey[33];    /* compressed SEC1 - ADR-0018 */
    uint8_t  confirm[16];       /* HMAC(confirm_key_hub, transcript), truncated */
} __attribute__((packed)) radio_pair_rsp_t;

/* Device -> hub, cleartext. Proves the device derived the same secret. */
typedef struct radio_pair_conf {
    uint8_t  type;              /* RADIO_FRAME_PAIR_CONF */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint8_t  confirm[16];       /* HMAC(confirm_key_dev, transcript), truncated */
} __attribute__((packed)) radio_pair_conf_t;

/* The sealed body of PAIR_ACCEPT.
 *
 * hop_key is the **network** hop key and is identical on every device. It
 * cannot be derived from the pairing secret: that secret is pairwise, so a hop
 * key derived from it gives every device a different permutation, and the hub
 * has one radio and sends one beacon per superframe on one channel. At most one
 * device could ever hear it. The failure is invisible with a single device on a
 * bench and presents as "the second device hears nothing", which reads like a
 * radio fault rather than a key. pair_v1's `pair_key_hop` is pinned and unused;
 * see docs/radio/pairing.md.
 *
 * Delivering it sealed under the session key is what keeps it secret: by then
 * the device has proved it holds the static key the operator vouched for.
 *
 * 19 bytes, and not being a multiple of four is deliberate. HAL_CRYP_Decrypt in
 * GCM mode does not mask the unused bytes of a partial final word, so such a
 * length fails the tag with byte-perfect ciphertext - which on air looks like a
 * radio fault. Making the first frame a device ever opens one of these means
 * the defect cannot wait to be found later. */
typedef struct radio_pair_grant {
    uint8_t slot;               /* uplink slot, 0..RADIO_SLOT_COUNT-1 */
    uint8_t report_every;       /* superframes between uplink reports */
    uint8_t flags;
    uint8_t hop_key[16];
} __attribute__((packed)) radio_pair_grant_t;

/* Hub -> device, sealed under the session key.
 *
 * rotate_epoch is deliberately absent: it is superframe / SUPERFRAME_PER_DAY
 * and both ends compute it from the counter already in this frame. A field
 * neither side needs to be told is a field neither side can be lied to about.
 *
 * AAD is the first 15 bytes - everything before `ct`. Expressed as an offset
 * rather than as a field list so there is nothing to get out of step with. */
typedef struct radio_pair_accept {
    uint8_t  type;              /* RADIO_FRAME_PAIR_ACCEPT */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint32_t superframe;        /* nonce input */
    uint8_t  retry;             /* nonce input: slot = RADIO_NONCE_SLOT_UNSLOTTED | retry */
    uint8_t  ct[19];            /* sealed radio_pair_grant_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_pair_accept_t;

#define RADIO_PAIR_ACCEPT_AAD_LEN  15u
#define RADIO_HOP_KEY_BYTES        16u

/* The sealed body of an uplink report. The first thing this network carries,
 * and the reason is that it measures the link itself: the device says how well
 * it hears the hub, the hub measures how well it hears the device, and a link
 * that is only good in one direction is then visible rather than mysterious. */
typedef struct radio_uplink_report {
    int8_t   rssi_down;         /* dBm, the hub's last data beacon as the device heard it */
    uint8_t  flags;             /* RADIO_REPORT_FLAG_* */
    /* Named for what is measured, not for what it is hoped to mean. A device
     * board with no battery reports its rail; on a battery-powered node the
     * rail is the battery. Either way the number is real. */
    uint16_t supply_mv;
    uint32_t uptime_s;          /* going backwards is how a reboot is seen */
} __attribute__((packed)) radio_uplink_report_t;

/* rssi_down carries the last value the device had rather than a sentinel, and
 * this says when that value is old - including before it has ever decoded a
 * beacon. A "rebooted" bit was the first candidate and is dominated by
 * uptime_s, which carries the same fact and survives a lost report better:
 * with no ack, a flag can only be cleared on transmit, so one lost frame
 * loses the reboot forever. */
#define RADIO_REPORT_FLAG_RSSI_STALE  0x01

/* Device -> hub, sealed under the session key, in the device's own slot.
 *
 * No dev_id on the wire: the hub assigned the slot, so it derives the identity
 * from it. That is not only four bytes - frames naming an *unassigned* slot are
 * rejected before any crypto runs. It is not a bound on work a stranger can
 * cause: transmitting in an assigned slot still buys a GCM open, and what
 * bounds that is the slot grid itself, one frame per slot per superframe.
 * The slot-to-device map is authoritative on the hub side, and a device changes
 * slots through a new PAIR_ACCEPT and by no other route.
 *
 * AAD is the first 7 bytes - everything before `ct`. */
typedef struct radio_uplink {
    uint8_t  type;              /* RADIO_FRAME_UPLINK */
    uint8_t  version;
    uint8_t  slot;
    uint32_t superframe;
    uint8_t  ct[8];             /* sealed radio_uplink_report_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_uplink_t;

#define RADIO_UPLINK_AAD_LEN  7u

/* --- the downlink ------------------------------------------------------
 *
 * The hub's half of the periodic exchange, and deliberately the uplink's mirror
 * image: same header shape, same AAD length, same 8-byte sealed body, same
 * addressing by slot. A second layout here would be a second parser, a second
 * nonce assembly and a second chance to disagree about where the slot byte
 * sits - and the slot byte is the field neither side's vectors cover.
 *
 * **Unicast, not broadcast.** It is sealed under one device's session key, so
 * unlike the data beacon it is authenticated: a forger cannot command a device
 * without the key that pairing established. That is the whole reason to spend a
 * region on it rather than widening the beacon.
 *
 * The nonce's direction byte is RADIO_DIR_DOWNLINK, which is what keeps a
 * downlink from ever colliding with an uplink under the same key, superframe
 * and slot. Same superframe, same slot, same key, different direction - and the
 * counter is the only thing preventing reuse, so the direction byte is not
 * decoration.
 *
 * **One device per opportunity.** The region is 25 000 us and a frame is about
 * 12 000, so the hub serves one device per downlink and round-robins. With N
 * devices a given one is addressed every 2N superframes, not every 2. Anything
 * that needs to reach every device promptly does not belong here.
 *
 * AAD is the first 7 bytes - everything before `ct`. */
typedef struct radio_downlink {
    uint8_t  type;              /* RADIO_FRAME_DOWNLINK */
    uint8_t  version;
    uint8_t  slot;              /* which device this is addressed to */
    uint32_t superframe;
    uint8_t  ct[8];             /* sealed radio_downlink_cmd_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_downlink_t;

#define RADIO_DOWNLINK_AAD_LEN  7u

/* The sealed body. `cmd` is the only field a future protocol needs to extend,
 * and an unknown one must be ignored rather than refused: a hub newer than a
 * device will send commands the device does not know, and a device that treats
 * that as an error stops honouring the keepalive it does understand. */
typedef struct radio_downlink_cmd {
    uint8_t  cmd;               /* RADIO_CMD_* */
    uint8_t  report_every;      /* superframes; 0 leaves the granted rate alone */
    uint16_t arg;
    uint32_t hub_time_s;        /* seconds since the hub booted, for device logs */
} __attribute__((packed)) radio_downlink_cmd_t;

enum {
    RADIO_CMD_NOP = 0,          /* the keepalive: the hub still holds this device */
    RADIO_CMD_SET_RATE,         /* honour report_every from the next superframe */
    RADIO_CMD_REJOIN            /* the hub has lost this device's keys; re-pair */
};

/* Same rule as the beacons above: literals, here, in the header both ends
 * compile. An assert comparing a struct to a length the same side defines is
 * the vacuous check in a different costume. */
_Static_assert(sizeof(radio_pair_rsp_t)    == 59, "pair response is 59 bytes on the wire");
_Static_assert(sizeof(radio_pair_conf_t)   == 26, "pair confirm is 26 bytes on the wire");
_Static_assert(sizeof(radio_pair_grant_t)  == 19, "pair grant is 19 bytes sealed");
_Static_assert(sizeof(radio_pair_accept_t) == 50, "pair accept is 50 bytes on the wire");
_Static_assert(sizeof(radio_uplink_report_t) == 8, "uplink report is 8 bytes sealed");
_Static_assert(sizeof(radio_uplink_t)      == 31, "uplink frame is 31 bytes on the wire");
_Static_assert(sizeof(radio_downlink_cmd_t) == 8, "downlink command is 8 bytes sealed");
_Static_assert(sizeof(radio_downlink_t)    == 31, "downlink frame is 31 bytes on the wire");
/* The two directions must stay the same shape. If one header grows, the offset
 * of `ct` moves on one side only and the AAD covers different bytes in each
 * direction - which opens correctly on the sender's own round trip and fails
 * against the other end, the failure that only a published frame catches. */
_Static_assert(offsetof(radio_downlink_t, ct) == offsetof(radio_uplink_t, ct),
               "the two directions disagree about where the sealed body starts");
_Static_assert(RADIO_DOWNLINK_AAD_LEN == RADIO_UPLINK_AAD_LEN,
               "the two directions disagree about what the AAD covers");

/* Nothing may exceed the hub's FIFO. Checked frame by frame rather than on the
 * largest, so the one that grows past 65 names itself: a frame over the ceiling
 * is refused by the transmitter, and a pairing frame that is never built is
 * indistinguishable on air from a hub that is deaf.
 *
 * This is the constraint pair_v3 is designed against - a hub-initiated first
 * frame carries an ephemeral point and a MAC, which is 49 bytes before any
 * addressing. */
#define RADIO_FITS(t) _Static_assert(sizeof(t) <= RADIO_MAX_PAYLOAD_B, \
                                     #t " does not fit the hub's FIFO")
RADIO_FITS(radio_data_beacon_t);
RADIO_FITS(radio_join_beacon_t);
RADIO_FITS(radio_pair_init_t);
RADIO_FITS(radio_pair_req_t);
RADIO_FITS(radio_pair_rsp_t);
RADIO_FITS(radio_pair_conf_t);
RADIO_FITS(radio_pair_accept_t);
RADIO_FITS(radio_uplink_t);
RADIO_FITS(radio_downlink_t);

/* And the same sizes as radio_slots.h charges air time for. Without this the
 * duty-cycle budget can be computed from a frame size that has since moved, and
 * every assert built on it stays green while describing frames that no longer
 * exist - the exchange budget was already carrying a 28 for a frame that is 50. */
_Static_assert(sizeof(radio_pair_init_t)   == RADIO_PAIR_INIT_BYTES,   "INIT air time");
_Static_assert(sizeof(radio_pair_req_t)    == RADIO_PAIR_REQ_BYTES,    "REQ air time");
_Static_assert(sizeof(radio_pair_rsp_t)    == RADIO_PAIR_RSP_BYTES,    "RSP air time");
_Static_assert(sizeof(radio_pair_conf_t)   == RADIO_PAIR_CONF_BYTES,   "CONF air time");
_Static_assert(sizeof(radio_pair_accept_t) == RADIO_PAIR_ACCEPT_BYTES, "ACCEPT air time");
_Static_assert(sizeof(radio_uplink_t)      == RADIO_UPLINK_BYTES,      "uplink air time");
_Static_assert(sizeof(radio_downlink_t)    == RADIO_DOWNLINK_BYTES,    "downlink air time");

/* The AAD lengths are offsets into these frames, so they have to move when the
 * frames do. Written as the distance to the sealed field rather than as a sum
 * of field sizes: a sum can stay correct while the offset it claims to describe
 * is wrong. */
_Static_assert(offsetof(radio_pair_accept_t, ct) == RADIO_PAIR_ACCEPT_AAD_LEN,
               "PAIR_ACCEPT AAD must cover exactly the cleartext header");
_Static_assert(offsetof(radio_uplink_t, ct) == RADIO_UPLINK_AAD_LEN,
               "uplink AAD must cover exactly the cleartext header");
