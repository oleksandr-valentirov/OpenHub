#pragma once

#include <stdint.h>
#include <stddef.h>

/* For the per-frame byte counts the slot budget is charged against.
 * radio_devices_docs/radio/tdma.md */
#include "radio_slots.h"
/* For RADIO_MAX_PAYLOAD_B, the hub FIFO ceiling every frame here must fit. */
#include "radio_phy.h"


typedef struct protocol_header {
    uint32_t hub_id;
    uint32_t dev_id;
} __attribute__((packed)) protocol_header_t;

typedef struct radio_pairing {
    protocol_header_t header;
    uint8_t stage;
} __attribute__((packed)) protocol_pairing_t;

/* Frame types. Only the join beacon is readable without a key. */
enum {
    RADIO_FRAME_DATA_BEACON = 0x01,
    RADIO_FRAME_JOIN_BEACON = 0x02,
    RADIO_FRAME_PAIR_REQ    = 0x03,   /**< device -> hub, join channel, cleartext */
    RADIO_FRAME_PAIR_RSP    = 0x04,   /**< hub -> device, join channel, cleartext */
    RADIO_FRAME_PAIR_CONF   = 0x05,   /**< device -> hub, join channel, cleartext */
    RADIO_FRAME_PAIR_ACCEPT = 0x06,   /**< hub -> device, sealed: the slot grant */
    RADIO_FRAME_UPLINK      = 0x07,   /**< device -> hub, sealed, in its own slot */
    RADIO_FRAME_DOWNLINK    = 0x08,   /**< hub -> device, sealed, in the downlink region */
    RADIO_FRAME_PAIR_INIT   = 0x09    /**< hub -> device, join channel, MACed (pair_v3) */
};

/* The version byte the pairing exchange carries. ADR-0012 */
#define RADIO_PROTO_VERSION          2u

/* The data frames alone: their bodies grew and pairing's did not.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_LINK_VERSION           4u

/* On the wire in four frame types, so both sides must compile the same one. */
#define RADIO_NET_ID                 0x0001u

#define RADIO_JOIN_FLAG_WINDOW_OPEN  0x01

/* The hub is about to leave the hop channels, announced in the data beacon.
 * radio_devices_docs/radio/decisions/0020-device-triggered-quiesce.md */
#define RADIO_BEACON_FLAG_QUIESCE    0x01

/* Every superframe on the hop channel, and frozen at 14 bytes.
 * radio_devices_docs/radio/tdma.md */
typedef struct radio_data_beacon {
    uint8_t  type;          /**< RADIO_FRAME_DATA_BEACON, like every other frame */
    uint8_t  version;
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t superframe;    /**< the protocol's clock, named for what it is */
    uint8_t  flags;
    uint8_t  resume_in;     /**< superframes until traffic resumes; 0 while running */
} __attribute__((packed)) radio_data_beacon_t;

/* Sent on the fixed join channel while a pairing window is open.
 * radio_devices_docs/radio/joining.md */
typedef struct radio_join_beacon {
    uint8_t  type;
    uint8_t  version;
    uint16_t net_id;        /**< public: it only has to identify the network */
    uint32_t hub_id;
    uint32_t superframe;
    uint8_t  flags;
    uint8_t  hop_channels;  /**< size of the hop set, so the plan needs no guessing */
} __attribute__((packed)) radio_join_beacon_t;

/* Against the contract's literals, never against sizeof itself.
 * See the `verification` skill. */
_Static_assert(sizeof(radio_data_beacon_t) == 14, "data beacon is 14 bytes on the wire");
_Static_assert(sizeof(radio_join_beacon_t) == 14, "join beacon is 14 bytes on the wire");

/* 3, while the rest of the exchange carries RADIO_PROTO_VERSION = 2. ADR-0021 */
#define RADIO_PAIR_INIT_VERSION  3u

/* pair_v3's invitation, addressed to a named device. ADR-0021 */
typedef struct radio_pair_init {
    uint8_t  type;              /**< RADIO_FRAME_PAIR_INIT */
    uint8_t  version;           /**< RADIO_PAIR_INIT_VERSION */
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t dev_id;            /**< addressed: only this device may answer */
    uint32_t superframe;
    uint8_t  mac[12];
} __attribute__((packed)) radio_pair_init_t;

_Static_assert(sizeof(radio_pair_init_t) == 28, "pair init is 28 bytes on the wire");
_Static_assert(RADIO_PAIR_INIT_VERSION != RADIO_PROTO_VERSION,
               "pair_v3's invitation is deliberately a different version byte "
               "from the pair_v2 frames that follow it");
_Static_assert(RADIO_LINK_VERSION != RADIO_PROTO_VERSION &&
               RADIO_LINK_VERSION != RADIO_PAIR_INIT_VERSION,
               "the three version bytes must stay tellable apart on the wire");
/* The split the MAC covers, written as an offset rather than a sum. */
#define RADIO_PAIR_INIT_MAC_LEN  12u
_Static_assert(offsetof(radio_pair_init_t, mac) ==
               sizeof(radio_pair_init_t) - RADIO_PAIR_INIT_MAC_LEN,
               "the MAC must cover exactly the cleartext ahead of it");

/* Device -> hub, join channel, cleartext; the only settled pairing frame.
 * radio_devices_docs/radio/pairing.md */
typedef struct radio_pair_req {
    uint8_t  type;          /**< RADIO_FRAME_PAIR_REQ */
    uint8_t  version;
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t dev_id;
    uint32_t superframe;
    uint8_t  dev_nonce[8];  /**< the device's only freshness; zero is refused */
    uint8_t  pubkey[33];    /**< compressed SEC1 point - ADR-0018 */
} __attribute__((packed)) radio_pair_req_t;

_Static_assert(sizeof(radio_pair_req_t) == 57, "pair request is 57 bytes on the wire");

/* The direction byte of the GCM nonce, pinned here and neither value zero.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
#define RADIO_DIR_UPLINK    0x01u   /* device -> hub */
#define RADIO_DIR_DOWNLINK  0x02u   /* hub -> device */

/* The nonce's slot field for frames in no uplink slot; low byte is a retry index.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
#define RADIO_NONCE_SLOT_UNSLOTTED  0xFFFF00u

/* Hub -> device, cleartext, in answer to a PAIR_REQ. No net_id, and dev_id
 * instead. radio_devices_docs/radio/pairing.md */
typedef struct radio_pair_rsp {
    uint8_t  type;              /**< RADIO_FRAME_PAIR_RSP */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint8_t  eph_pubkey[33];    /**< compressed SEC1 - ADR-0018 */
    uint8_t  confirm[16];       /**< HMAC(confirm_key_hub, transcript), truncated */
} __attribute__((packed)) radio_pair_rsp_t;

/* Device -> hub, cleartext. Proves the device derived the same secret. */
typedef struct radio_pair_conf {
    uint8_t  type;              /**< RADIO_FRAME_PAIR_CONF */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint8_t  confirm[16];       /**< HMAC(confirm_key_dev, transcript), truncated */
} __attribute__((packed)) radio_pair_conf_t;

/* Sealed PAIR_ACCEPT body; hop_key is the NETWORK key, 19 bytes on purpose.
 * radio_devices_docs/radio/pairing.md */
typedef struct radio_pair_grant {
    uint8_t slot;               /**< uplink slot, 0..RADIO_SLOT_COUNT-1 */
    uint8_t report_every;       /**< superframes between uplink reports */
    uint8_t flags;
    uint8_t hop_key[16];
} __attribute__((packed)) radio_pair_grant_t;

/* Hub -> device, sealed under the session key; AAD is everything before ct.
 * radio_devices_docs/radio/crypto/key-lifecycle.md */
typedef struct radio_pair_accept {
    uint8_t  type;              /**< RADIO_FRAME_PAIR_ACCEPT */
    uint8_t  version;
    uint32_t hub_id;
    uint32_t dev_id;
    uint32_t superframe;        /**< nonce input */
    uint8_t  retry;             /**< nonce input: slot = RADIO_NONCE_SLOT_UNSLOTTED | retry */
    uint8_t  ct[19];            /**< sealed radio_pair_grant_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_pair_accept_t;

#define RADIO_PAIR_ACCEPT_AAD_LEN  15u
#define RADIO_HOP_KEY_BYTES        16u

/* The sealed body of an uplink report, which measures the link in both directions. */
typedef struct radio_uplink_report {
    int8_t   rssi_down;         /**< dBm, the hub's last data beacon as the device heard it */
    uint8_t  flags;             /**< RADIO_REPORT_FLAG_* */
    uint16_t supply_mv;         /**< the rail, which may or may not be a battery */
    uint32_t uptime_s;          /**< device seconds; wraps every 71.6 min, so not a reboot signal */
    uint8_t  ack_seq;           /**< the cmd_seq this device last applied */
    uint8_t  ack_cmd;           /**< what it was, so a mismatched ack is visible */
    uint8_t  app_len;           /**< 0 means no application data, never a sentinel */
    uint8_t  app[5];
} __attribute__((packed)) radio_uplink_report_t;

/* Says when rssi_down is stale, since it carries a last value, not a sentinel. */
#define RADIO_REPORT_FLAG_RSSI_STALE  0x01

/* Device -> hub, sealed, in its own slot and carrying no dev_id.
 * radio_devices_docs/radio/tdma.md */
typedef struct radio_uplink {
    uint8_t  type;              /**< RADIO_FRAME_UPLINK */
    uint8_t  version;
    uint8_t  slot;
    uint32_t superframe;
    uint8_t  ct[16];            /**< sealed radio_uplink_report_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_uplink_t;

#define RADIO_UPLINK_AAD_LEN  7u

/* --- the downlink ------------------------------------------------------ */

/* Unicast, sealed, and the uplink's mirror image, one device per opportunity.
 * radio_devices_docs/radio/tdma.md */
typedef struct radio_downlink {
    uint8_t  type;              /**< RADIO_FRAME_DOWNLINK */
    uint8_t  version;
    uint8_t  slot;              /**< which device this is addressed to */
    uint32_t superframe;
    uint8_t  ct[16];            /**< sealed radio_downlink_cmd_t */
    uint8_t  tag[16];
} __attribute__((packed)) radio_downlink_t;

#define RADIO_DOWNLINK_AAD_LEN  7u

/* Never issued, so ack_seq 0 means "nothing applied" and cannot mean an echo.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_CMD_SEQ_NONE  0u

/* The sealed body. An unknown cmd must be ignored, never refused.
 * radio_devices_docs/radio/tdma.md */
typedef struct radio_downlink_cmd {
    uint8_t  cmd;               /**< RADIO_CMD_* */
    uint8_t  report_every;      /**< superframes; 0 leaves the granted rate alone */
    uint16_t arg;
    uint32_t hub_time_s;        /**< seconds since the hub booted, for device logs */
    uint8_t  cmd_seq;           /**< RADIO_CMD_SEQ_NONE, or 1..255 naming this command */
    uint8_t  app_len;           /**< 0 means no application data, never a sentinel */
    uint8_t  app[6];
} __attribute__((packed)) radio_downlink_cmd_t;

enum {
    RADIO_CMD_NOP = 0,          /**< the keepalive: the hub still holds this device */
    RADIO_CMD_SET_RATE,         /**< honour report_every from the next superframe */
    RADIO_CMD_REJOIN            /**< the hub has lost this device's keys; re-pair */
};

/* Literals, here, in the header both ends compile. ADR-0012 */
_Static_assert(sizeof(radio_pair_rsp_t)    == 59, "pair response is 59 bytes on the wire");
_Static_assert(sizeof(radio_pair_conf_t)   == 26, "pair confirm is 26 bytes on the wire");
_Static_assert(sizeof(radio_pair_grant_t)  == 19, "pair grant is 19 bytes sealed");
_Static_assert(sizeof(radio_pair_accept_t) == 50, "pair accept is 50 bytes on the wire");
_Static_assert(sizeof(radio_uplink_report_t) == 16, "uplink report is 16 bytes sealed");
_Static_assert(sizeof(radio_uplink_t)      == 39, "uplink frame is 39 bytes on the wire");
_Static_assert(sizeof(radio_downlink_cmd_t) == 16, "downlink command is 16 bytes sealed");
_Static_assert(sizeof(radio_downlink_t)    == 39, "downlink frame is 39 bytes on the wire");
/* A length field that can name more than its array is a read past the end.
 * radio_devices_docs/radio/tdma.md */
_Static_assert(sizeof(((radio_uplink_report_t *)0)->app) < 256u &&
               sizeof(((radio_downlink_cmd_t *)0)->app) < 256u,
               "app_len is a uint8 and must be able to name the whole array");

/* The two directions must stay the same shape.
 * radio_devices_docs/radio/tdma.md */
_Static_assert(offsetof(radio_downlink_t, ct) == offsetof(radio_uplink_t, ct),
               "the two directions disagree about where the sealed body starts");
_Static_assert(RADIO_DOWNLINK_AAD_LEN == RADIO_UPLINK_AAD_LEN,
               "the two directions disagree about what the AAD covers");

/* Nothing may exceed the hub's FIFO, checked frame by frame so the offender names
 * itself. radio_devices_docs/radio/tdma.md */
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

/* And the same sizes radio_slots.h charges air time for.
 * radio_devices_docs/radio/tdma.md */
_Static_assert(sizeof(radio_pair_init_t)   == RADIO_PAIR_INIT_BYTES,   "INIT air time");
_Static_assert(sizeof(radio_pair_req_t)    == RADIO_PAIR_REQ_BYTES,    "REQ air time");
_Static_assert(sizeof(radio_pair_rsp_t)    == RADIO_PAIR_RSP_BYTES,    "RSP air time");
_Static_assert(sizeof(radio_pair_conf_t)   == RADIO_PAIR_CONF_BYTES,   "CONF air time");
_Static_assert(sizeof(radio_pair_accept_t) == RADIO_PAIR_ACCEPT_BYTES, "ACCEPT air time");
_Static_assert(sizeof(radio_uplink_t)      == RADIO_UPLINK_BYTES,      "uplink air time");
_Static_assert(sizeof(radio_downlink_t)    == RADIO_DOWNLINK_BYTES,    "downlink air time");

/* AAD lengths are offsets into these frames, so they move when the frames do. */
_Static_assert(offsetof(radio_pair_accept_t, ct) == RADIO_PAIR_ACCEPT_AAD_LEN,
               "PAIR_ACCEPT AAD must cover exactly the cleartext header");
_Static_assert(offsetof(radio_uplink_t, ct) == RADIO_UPLINK_AAD_LEN,
               "uplink AAD must cover exactly the cleartext header");
