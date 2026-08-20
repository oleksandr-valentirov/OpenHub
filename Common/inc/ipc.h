#pragma once

#include <stdint.h>
#include <stddef.h>

#include "shared_memory.h"

/* Cross-core mailbox between CM7 (application) and CM4 (radio).
 *
 * Two single-producer/single-consumer rings in SRAM4. The MPU maps that region
 * strongly-ordered on CM7, so head and tail need a barrier but no cache
 * maintenance, and one writer per index means no semaphore on the data itself.
 * HSEM only wakes the far side; losing a pulse costs latency, not a message,
 * because the consumer polls the ring rather than trusting the flag.
 *
 * Every request carries a sequence number that its reply echoes. Without one, a
 * late reply to a request that already timed out is indistinguishable from the
 * reply to the next request - the single-slot mailbox this replaces got that
 * wrong by construction. */

#define IPC_MAGIC        0x4F484231u   /* 'OHB1' - both cores must agree */
/* 2 added the CM4 -> CM7 event rings. The version gate is what makes a
 * half-flashed board fail loudly: CM7 stamps the header before releasing CM4,
 * so an old CM4 against a new CM7 refuses every message instead of reading a
 * ring at the wrong offset. */
#define IPC_VERSION      2u
#define IPC_RING_SLOTS   8u            /* power of two */
/* The largest PAIR_INIT CM4 will hold. radio_protocol.h owns the real size;
 * this is the buffer, deliberately a little larger so a frame that grows does
 * not silently truncate here - the assert in radio_protocol.h is what catches
 * a frame that no longer fits the radio. */
#define RADIO_PAIR_INIT_MAX  32u
#define IPC_PAYLOAD_MAX  64u

/* requests, CM7 -> CM4 */
enum {
    IPC_REQ_NONE = 0,
    IPC_REQ_READ_REG,
    IPC_REQ_ADD_DEVICE,
    IPC_REQ_REMOVE_DEVICE,
    IPC_REQ_GET_DEVICE_INFO,
    IPC_REQ_SET_DEVICE_PARAM,
    IPC_REQ_GET_TIMING,
    IPC_REQ_HOP_PRF,
    IPC_REQ_GET_PAIR_STATE,
    IPC_REQ_QUIESCE,         /* test scaffolding: force a quiesce with no device */
    IPC_REQ_GET_STORE,
    IPC_REQ_KV_TORN,         /* test scaffolding: a bad record in the counter log */
    IPC_REQ_INSTALL_DEVICE,  /* the keys and slot of an already-paired device */
    IPC_REQ_SET_REPORT_RATE, /* what future pairings grant, in superframes */
    IPC_REQ_GET_EXCHANGE,    /* the four-frame exchange's own counters */
    IPC_REQ_GET_VECTORS,     /* which vector sets the radio core was built against */
    IPC_REQ_GET_RXDIAG,      /* what the radio is hearing, below the frame layer */
    /* Loop a pattern through the radio's FIFO and back. Nothing on air, no
     * device needed: it isolates this core's SPI read path from everything
     * above it, which no test that needs a frame can do. */
    IPC_REQ_SPI_LOOP,
    IPC_REQ_HOP_AT,         /* which channel a given superframe lands on */
    IPC_REQ_GET_DOWNLINK,   /* the hub's half of the periodic exchange */
    /* A PAIR_INIT built and MACed by CM7, for CM4 to key at a named superframe.
     * CM4 has no SHA-256 and this frame's MAC is HMAC-SHA256, so the crypto
     * stays where the vectors and the self-tests are and CM4 stays a radio. */
    IPC_REQ_SET_PAIR_INIT,
    IPC_REQ_GET_PAIR_INIT   /* what CM4 did with the ones it was given */
};

/* Events, CM4 -> CM7. A separate ring and a separate number space: these are
 * requests the *radio* originates, and it originates them because a device
 * transmitted something. Polling for them from CM7 instead would make the
 * mailbox work continuously to represent an event that happens when a human
 * presses a button on a sensor.
 *
 * The exchange runs on CM7 because P-256 does. See ADR-0011. */
enum {
    IPC_EVT_NONE = 0,
    IPC_EVT_PAIR_REQ,        /* a device answered the join beacon */
    IPC_EVT_PAIR_CONF        /* ... and confirmed the derived secret */
};

/* reply status */
enum {
    IPC_ST_OK = 0,
    IPC_ST_UNKNOWN_REQ,
    IPC_ST_BAD_ARG,
    IPC_ST_RADIO_ERR
};

/* Reply payload for IPC_REQ_GET_TIMING. Beacon lateness against the superframe
 * boundary: devices measure the period from beacon to beacon, so this jitter
 * lands directly in every device's estimate and from there into its drift. */
typedef struct ipc_timing {
    uint32_t superframe;
    uint32_t now_tk;         /* raw TIM2, so the tick rate is checkable from the console */
    uint32_t late_last_us;
    uint32_t late_max_us;
    uint32_t late_min_us;
    uint32_t period_us;      /* nominal, what the schedule is written in */
    uint32_t period_tk;      /* what the grid actually steps, after calibration */
    int32_t  calib_ppm;      /* timer clock offset from nominal */
    int32_t  calib_ppm_min;  /* spread across windows; wide means a bad measurement */
    int32_t  calib_ppm_max;
    uint32_t span_lo;        /* per-capture span extremes, last window */
    uint32_t span_hi;
    uint32_t calib_windows;
    uint32_t calib_rejects;
    uint32_t late_over;      /* beacons that left later than the cost limit */
    /* Ticks since the last accepted window. calib_windows only ever says one
     * landed, once, so a stopped reference leaves the last measured scale being
     * reported as current with nothing saying how old it is. */
    uint32_t calib_age_tk;
} __attribute__((packed)) ipc_timing_t;

/* The radio's pairing state machine. Shared because the CLI names these back to
 * an operator, and a number the operator has to look up in another file is a
 * number that gets misread. */
typedef enum radio_pair_state {
    RADIO_PAIR_IDLE = 0,   /* grid running, nothing on the join channel */
    RADIO_PAIR_LISTEN,     /* window open: join beacon and RX in the join region */
    RADIO_PAIR_QUIESCE     /* announced on air; grid silent, hub on the join channel */
} radio_pair_state_t;

/* The four-frame exchange, which is a different machine from the schedule
 * above: pair_state says whether the grid is suspended, this says how far the
 * conversation with one device has got. */
typedef enum radio_exchange_state {
    RADIO_EX_IDLE = 0,
    RADIO_EX_WAIT_RSP,    /* PAIR_REQ forwarded; CM7 is doing the curve work */
    RADIO_EX_SENT_RSP,    /* PAIR_RSP is on air; waiting for PAIR_CONF */
    RADIO_EX_WAIT_KEYS,   /* PAIR_CONF forwarded; CM7 is checking it */
    RADIO_EX_ACCEPTED     /* PAIR_ACCEPT sent; the device is installed */
} radio_exchange_state_t;

/* Reply payload for IPC_REQ_GET_PAIR_STATE. */
/* Why the last beacon failed, and why the last join frame was dropped.
 *
 * Both counters fold several distinct outcomes into one number - four and seven
 * respectively - and a count that cannot say which is a count that only tells
 * an operator that something is wrong. Full counters do not fit the payload, so
 * what is carried is the *most recent* reason, which costs the two bytes that
 * were already reserved. Found by grepping for counters incremented at more
 * than one site, which is the device side's rule: every silent path needs the
 * split, including the ones nobody has told a story about yet. */
enum {
    RADIO_BERR_NONE = 0,
    RADIO_BERR_BUILD,        /* payload would not fit the frame buffer */
    RADIO_BERR_PRF,          /* the hop PRF failed - no channel, so no transmit */
    RADIO_BERR_RETUNE,
    RADIO_BERR_TX
};

enum {
    RADIO_DROP_NONE = 0,
    RADIO_DROP_VERSION,      /* too short, or a protocol version we do not speak */
    RADIO_DROP_TYPE,         /* a frame type that has no business on this channel */
    RADIO_DROP_LEN,          /* right type, wrong length - a contract mismatch */
    RADIO_DROP_IDS,          /* kept for the wire; the three below replace it */
    RADIO_DROP_NO_WINDOW,    /* nobody has pressed the button; not a fault */
    RADIO_DROP_BUSY,         /* an exchange is already in flight */
    /* One counter for net_id, hub_id and dev_id said "ids" and sent the search
     * nowhere: the three fail for entirely different reasons and one of them is
     * an operator opening a window for a different device. */
    RADIO_DROP_NET_ID,
    RADIO_DROP_HUB_ID,
    RADIO_DROP_DEV_ID
};
#define RADIO_DROP_COUNT 10u

typedef struct ipc_pair_state {
    uint8_t  state;          /* radio_pair_state_t */
    uint8_t  quiesce_left;   /* superframes until the grid resumes */
    uint8_t  beacon_err_last;  /* RADIO_BERR_* */
    uint8_t  reqs_drop_last;   /* RADIO_DROP_* */
    uint32_t dev_id;         /* the device the window was opened for */
    uint32_t window_left_ms;
    uint32_t resume_at;      /* superframe the hub has committed to resuming on */
    uint32_t reqs_seen;      /* PAIR_REQ frames received on the join channel */
    uint32_t reqs_dropped;   /* ... and rejected: wrong id, bad length, no window */
    uint32_t join_regions;   /* join regions entered */
    uint32_t join_beacons;   /* join beacons handed to the radio */
    uint32_t join_tx_err;    /* ... of which the driver refused or timed out */
    /* What the hub actually put on the hop channels, so the state machine can
     * be checked without a clean spectrum: data_beacons + silent_frames must
     * equal the superframes elapsed, and announce_beacons must be
     * RADIO_QUIESCE_ANNOUNCE per quiesce. */
    uint32_t data_beacons;
    uint32_t announce_beacons;
    uint32_t silent_frames;
    uint32_t quiesce_refused;   /* asked for one inside the minimum gap */
    uint32_t beacon_err;        /* beacons the radio would not put on air */
    uint32_t quiesce_lost;      /* valid PAIR_REQs that won no clear air */
} __attribute__((packed)) ipc_pair_state_t;

/* Reply payload for IPC_REQ_GET_STORE. The durable superframe ceiling and how
 * much log is left to write it into. */
typedef struct ipc_store_state {
    uint32_t reserved;    /* nothing at or below this may be reused */
    uint32_t counter;     /* where the counter is now, so the margin is visible */
    uint32_t writes;
    uint32_t errors;      /* non-zero means the ceiling has stopped advancing */
    uint32_t slots_left;
    uint32_t unreserved;  /* superframes the radio stayed silent to avoid reuse */
} __attribute__((packed)) ipc_store_state_t;

/* Every reply payload must fit the slot that carries it. Exceeding it is not a
 * compile error by itself: the reply is simply truncated, the far side's length
 * check refuses it, and the symptom is "CM4 rejected it, status 0" - which says
 * nothing about size. That has already cost this project one debugging session,
 * so the constraint is asserted where the structs are defined. */
_Static_assert(sizeof(ipc_timing_t)     <= IPC_PAYLOAD_MAX, "ipc_timing_t too large");
_Static_assert(sizeof(ipc_pair_state_t) <= IPC_PAYLOAD_MAX, "ipc_pair_state_t too large");
_Static_assert(sizeof(ipc_store_state_t) <= IPC_PAYLOAD_MAX, "ipc_store_state_t too large");
/* IPC_EVT_PAIR_REQ: what CM4 pulled out of a PAIR_REQ frame. CM4 has already
 * checked net_id, hub_id, the window and the device the operator named, so what
 * crosses the boundary is only what CM7 needs to do arithmetic with. */
typedef struct ipc_pair_req_evt {
    uint32_t dev_id;
    uint32_t superframe;     /* the counter the device echoed back */
    uint8_t  dev_nonce[8];   /* the device's contribution of freshness */
    uint8_t  pubkey[33];     /* the device's static key, compressed SEC1 */
} __attribute__((packed)) ipc_pair_req_evt_t;

/* ... and its reply: the body of PAIR_RSP. CM4 frames it and puts it on air. */
typedef struct ipc_pair_rsp_evt {
    uint8_t eph_pubkey[33];
    uint8_t confirm[16];
} __attribute__((packed)) ipc_pair_rsp_evt_t;

/* IPC_EVT_PAIR_CONF: the device's confirmation, for CM7 to check against the
 * transcript it still holds. */
typedef struct ipc_pair_conf_evt {
    uint32_t dev_id;
    uint8_t  confirm[16];
} __attribute__((packed)) ipc_pair_conf_evt_t;

/* Everything CM4 needs to serve one paired device.
 *
 * One struct for two paths on purpose: it is the reply to IPC_EVT_PAIR_CONF
 * when a pairing completes, and the payload of IPC_REQ_INSTALL_DEVICE when CM7
 * replays the keystore into a freshly booted CM4. The two carry identical
 * information, and a second struct saying the same thing is a second thing to
 * keep in step.
 *
 * hop_key is the network key, identical for every device - see
 * radio_pair_grant_t for why it cannot be derived from the pairwise secret. */
typedef struct ipc_device_keys {
    uint32_t dev_id;
    uint32_t key_gen;
    uint8_t  session_key[16];
    uint8_t  hop_key[16];
    uint8_t  slot;
    uint8_t  report_every;
    uint8_t  flags;
    uint8_t  reserved;
} __attribute__((packed)) ipc_device_keys_t;

/* Reply payload for IPC_REQ_GET_EXCHANGE. Separate from ipc_pair_state_t
 * because that one is full: a reply that overflows IPC_PAYLOAD_MAX is silently
 * truncated and the far side refuses it with a status that says nothing about
 * size. The exchange counters are also a different question from the schedule's
 * - one is "did the four frames happen", the other is "is the grid running". */
typedef struct ipc_exchange_state {
    uint8_t  state;            /* radio_exchange_state_t */
    uint8_t  aead_selftest;    /* 0 = passed; otherwise the failing check */
    uint8_t  devices;          /* installed on the radio core */
    uint8_t  report_every;     /* granted to the next pairing */
    uint32_t dev_id;           /* the exchange in flight, if any */
    uint32_t reqs_forwarded;   /* PAIR_REQ handed to CM7 */
    uint32_t rsp_sent;
    uint32_t confs_forwarded;
    uint32_t accepts_sent;
    uint32_t paired;
    /* Refusal counts, 16-bit: this struct was already exactly at
     * IPC_PAYLOAD_MAX and the two uplink counters below had to come from
     * somewhere. A hub that refuses 65535 exchanges has a different problem
     * than the one this number would help diagnose. */
    uint16_t cm7_refused;      /* CM7 answered, and said no */
    uint16_t timeouts;         /* CM7 or the device never answered */
    uint16_t tx_err;
    uint16_t seal_err;
    uint32_t uplink_frames;    /* frames received in the uplink region */
    uint32_t uplink_ok;        /* ... whose tag verified */
    /* 16 bits: error counters, not traffic counters. Narrowed to make room for
     * uplink_replay when the IPC_PAYLOAD_MAX assert refused the struct - which
     * is the second time that assert has caught exactly this. */
    uint16_t uplink_bad_slot;  /* ... naming a slot with no device */
    /* Split apart deliberately. "Malformed" and "the tag did not verify" are
     * the difference between the air being wrong and the key being wrong, and
     * one number for both names neither - the same distinction the device side
     * built for its own receive path. */
    uint16_t uplink_bad_frame; /* wrong length, type or version */
    uint16_t uplink_bad_tag;   /* well formed, and it did not authenticate */
    /* Authenticated and refused anyway: not newer than the device's floor. A
     * genuine device never produces one, so any value here is either a replay
     * or a device whose counter went backwards - both worth seeing. */
    uint16_t uplink_replay;
    /* Receive windows actually opened, and sync detections inside them. Without
     * these, "uplink 0 seen" cannot tell a device that transmitted nothing from
     * a hub whose receiver was never on - and the window is gated on a paired
     * device, a quiesce and a pairing window, so it not opening is likely. */
    uint32_t uplink_windows;
    uint32_t uplink_sync;
} __attribute__((packed)) ipc_exchange_state_t;

/* Reply payload for IPC_REQ_GET_RXDIAG: what the radio hears, below the layer
 * that has an opinion about frames.
 *
 * Its own message rather than more fields on ipc_pair_state_t, which the
 * payload assert refused - and rightly, because this answers a different
 * question. "Is the grid running" and "is anything arriving at all" are the two
 * halves an operator has to separate when nothing works.
 *
 * The three counters together are the diagnosis:
 *
 *   sync 0, crc 0             nothing detected - frequency, sync word,
 *                             preamble or range
 *   sync N, crc N             frames arriving and failing the checksum
 *   sync N, crc 0, frames N   the radio layer is working
 *
 * Without CrcAutoClearOff the SX1231 discards a bad frame before PayloadReady,
 * so the first two cases were indistinguishable and both read as an empty band.
 * The device side's SX126x reports a distinct CRC error, which made its
 * receiver the only observable point in the network until this existed. */
typedef struct ipc_rx_diag {
    uint32_t sync_match;    /* rising edges of SyncAddressMatch */
    uint32_t crc_err;       /* delivered by the part, failed its CRC */
    uint32_t frames;        /* delivered with the CRC verified */
    uint32_t last_superframe;
    uint8_t  last_len;      /* recorded before anything is judged about it */
    uint8_t  last_type;
    int8_t   last_rssi;
    /* The strongest signal seen while the receiver was open, and the quietest -
     * sampled from RegRssiValue, which is *below the sync word*.
     *
     * sync_match already sits below CRC, but it still requires the sync word to
     * match. If a transmission arrives whose sync word this part does not
     * recognise, every counter above reads zero and the band looks empty. RSSI
     * is the only measurement that answers "is anything being radiated at me at
     * all", and it is the difference between "your frame is not arriving" and
     * "your frame arrives and I do not recognise it" - two conclusions that
     * send the search in opposite directions. */
    int8_t   rssi_peak;     /* dBm, floor when nothing has been heard */
    int8_t   rssi_floor;
    /* The same measurement taken inside the uplink slot region instead of the
     * join window. sync 0 there means the same two things it meant on the join
     * channel - nothing radiated, or something radiated this part cannot read -
     * and only a level tells them apart. */
    int8_t   up_rssi_peak;
    int8_t   up_rssi_floor;
    uint8_t  reserved[1];
    /* Measurements actually taken. peak and floor read 0 both when the window
     * never opened and when every trigger failed, and those send the search in
     * opposite directions - same reason the two above exist. */
    uint32_t rssi_samples;
    /* The identifiers the last refused PAIR_REQ carried. Naming which field
     * mismatched still leaves two implementations each certain they wrote the
     * same number; the value is what settles it in one read. */
    uint32_t drop_hub_id;
    uint32_t drop_dev_id;
    uint16_t drop_net_id;
    uint8_t  reserved2[2];
    /* The head of the last PAIR_REQ as it came off the FIFO, and the head of
     * the public key inside it. Decoded fields are already an interpretation -
     * two sides comparing their own structs can agree on every field and still
     * disagree about the bytes.
     *
     * Recorded for every request, not only refused ones: an instrument that
     * fires on one refusal path goes silent exactly when that path stops
     * failing, which is when the next fault starts. */
    uint8_t  drop_head[16];
    uint8_t  drop_key[8];    /* frame offset 24..31 */
} __attribute__((packed)) ipc_rx_diag_t;

/* Request payload for IPC_REQ_SET_PAIR_INIT.
 *
 * The frame is opaque to CM4 - it neither builds nor checks the MAC, it keys
 * the bytes at the superframe it is told. Carrying the superframe separately
 * rather than parsing it out of the frame is deliberate: CM4 would then be
 * reading a field whose position it has no other reason to know, and a layout
 * change would move a transmit schedule instead of breaking a build. */
typedef struct ipc_pair_init {
    uint32_t superframe;     /* key it when the counter reads exactly this */
    uint8_t  len;
    uint8_t  frame[RADIO_PAIR_INIT_MAX];
} __attribute__((packed)) ipc_pair_init_t;
_Static_assert(sizeof(ipc_pair_init_t) <= IPC_PAYLOAD_MAX, "ipc_pair_init_t too large");

/* Reply payload for IPC_REQ_GET_PAIR_INIT. Every way one can fail to reach the
 * air, because "0 sent" reads the same for a frame that arrived too late, one
 * whose superframe had already passed, and one that was never pushed. */
typedef struct ipc_pair_init_state {
    uint32_t given;          /* frames accepted from CM7 */
    uint32_t sent;
    uint32_t missed;         /* its superframe passed before the region opened */
    uint32_t tx_err;
    uint32_t replaced;       /* a new frame arrived while one was still pending */
    uint32_t last_sent_sf;
    uint32_t pending_sf;     /* 0 when nothing is queued */
} __attribute__((packed)) ipc_pair_init_state_t;
_Static_assert(sizeof(ipc_pair_init_state_t) <= IPC_PAYLOAD_MAX,
               "ipc_pair_init_state_t too large");

/* Reply payload for IPC_REQ_HOP_AT.
 *
 * The key travels with the answer. A channel number is only comparable between
 * two implementations if both name the key they computed it under - the device
 * side asked its hop command for a channel and got one derived from the
 * published test vector's key, which is a correct answer to a question nobody
 * asked and indistinguishable from the right one. */
typedef struct ipc_hop_at {
    uint32_t superframe;
    uint8_t  channel;        /* index into the hop set */
    uint8_t  grid_slot;      /* ... mapped past the reserved join slot */
    uint8_t  placeholder;    /* 1 while CM7 has not installed the network key */
    uint8_t  key_head[4];
    uint32_t hz;             /* the carrier that slot maps to */
    uint8_t  count;          /* the hop set this deck was built over */
    uint8_t  deck[32];       /* the permutation itself, not a channel from it */
} __attribute__((packed)) ipc_hop_at_t;
_Static_assert(sizeof(ipc_hop_at_t) <= IPC_PAYLOAD_MAX, "ipc_hop_at_t too large");

/* Reply payload for IPC_REQ_GET_DOWNLINK.
 *
 * Its own message rather than more fields on ipc_exchange_state_t, which is
 * already exactly at IPC_PAYLOAD_MAX - the assert caught that once tonight
 * after a build reported as succeeding had in fact failed. */
typedef struct ipc_downlink_state {
    uint32_t opportunities;  /* superframes whose parity said a downlink is due */
    uint32_t sent;
    uint32_t seal_err;
    uint32_t tx_err;
    uint32_t no_device;      /* the rotation found nobody, with devices installed */
    uint32_t prf_err;        /* no hop channel, so nothing was transmitted */
    /* The carrier the last downlink actually left on. A send counter cannot see
     * a wrong channel - 93 frames went out on the join channel reading 93/93. */
    uint32_t last_hz;
    uint32_t last_superframe;
    uint8_t  next_slot;      /* where the round robin resumes */
    uint8_t  reserved[3];
} __attribute__((packed)) ipc_downlink_state_t;
_Static_assert(sizeof(ipc_downlink_state_t) <= IPC_PAYLOAD_MAX,
               "ipc_downlink_state_t too large");

/* Reply payload for IPC_REQ_SPI_LOOP. */
typedef struct ipc_spi_loop {
    /* Two paths, because a FIFO loopback that has never once come back clean
     * cannot tell "the bus is corrupting bytes" from "this is not a valid use
     * of the FIFO". A register block the part is guaranteed to hold verbatim
     * is the control the FIFO test does not have on its own. */
    uint32_t reg_bad_bytes;
    uint32_t reg_xor_80;
    uint8_t  reg_read[8];
    uint32_t passes;
    uint32_t bad_bytes;      /* bytes that came back different */
    uint32_t bad_passes;     /* passes with at least one */
    uint32_t xor_80;         /* ... of which the difference was bit 7 alone */
    uint8_t  first_bad[8];   /* what came back, from the first failing pass */
    uint8_t  expect[8];      /* and what was written */
    uint32_t io_err;
    /* The bus rate this ran at, read out of the peripheral rather than derived
     * from the prescaler: a divider is meaningless without its kernel clock,
     * and the kernel clock is a PLL setting three files away. */
    uint32_t spi_hz;
} __attribute__((packed)) ipc_spi_loop_t;
_Static_assert(sizeof(ipc_spi_loop_t) <= IPC_PAYLOAD_MAX, "ipc_spi_loop_t too large");

/* Reply payload for IPC_REQ_GET_VECTORS.
 *
 * The digests existed for an hour with no consumer at all: three generators
 * computed them, three headers carried them, and nothing read one. Decorative -
 * correct, and acted on by nobody - which is the second failure class in this
 * project's own list, sitting in the thing that was being carefully perfected.
 *
 * This is a consumer that can fail. The two cores are flashed separately and
 * this project has already been bitten by flashing one and not the other; a
 * digest mismatch between them says exactly that, and says it before the radio
 * is trusted rather than after a pairing fails on air. */
typedef struct ipc_vectors {
    char    pair[17];        /* PAIR_VECTORS_DIGEST, NUL-terminated */
    char    hop[17];         /* HOP_VECTORS_DIGEST */
    uint8_t pair_version;
    uint8_t reserved[3];
} __attribute__((packed)) ipc_vectors_t;

/* Reply payload for IPC_REQ_GET_DEVICE_INFO, which asks for the index-th live
 * device. `total` rides along so a caller learns how many there are from the
 * first answer rather than probing until it is refused. */
typedef struct ipc_device_report {
    uint32_t dev_id;
    uint32_t last_superframe;  /* of the last report whose tag verified */
    uint32_t frames_ok;
    uint32_t frames_bad;       /* wrong length, wrong version, or a failed tag */
    uint32_t uptime_s;         /* as the device reported it */
    uint8_t  total;            /* live devices, so one round trip sizes the list */
    uint8_t  slot;
    int8_t   rssi_up;          /* dBm, measured here, on the device's last frame */
    int8_t   rssi_down;        /* dBm, as the device heard the hub's last beacon */
    uint16_t supply_mv;
    uint8_t  report_every;
    uint8_t  flags;            /* RADIO_REPORT_FLAG_* from the last report */
} __attribute__((packed)) ipc_device_report_t;

_Static_assert(sizeof(ipc_pair_req_evt_t)  <= IPC_PAYLOAD_MAX, "ipc_pair_req_evt_t too large");
_Static_assert(sizeof(ipc_pair_rsp_evt_t)  <= IPC_PAYLOAD_MAX, "ipc_pair_rsp_evt_t too large");
_Static_assert(sizeof(ipc_pair_conf_evt_t) <= IPC_PAYLOAD_MAX, "ipc_pair_conf_evt_t too large");
_Static_assert(sizeof(ipc_device_keys_t)   <= IPC_PAYLOAD_MAX, "ipc_device_keys_t too large");
_Static_assert(sizeof(ipc_device_report_t) <= IPC_PAYLOAD_MAX, "ipc_device_report_t too large");
_Static_assert(sizeof(ipc_exchange_state_t) <= IPC_PAYLOAD_MAX, "ipc_exchange_state_t too large");
_Static_assert(sizeof(ipc_vectors_t) <= IPC_PAYLOAD_MAX, "ipc_vectors_t too large");
_Static_assert(sizeof(ipc_rx_diag_t) <= IPC_PAYLOAD_MAX, "ipc_rx_diag_t too large");

typedef struct ipc_msg {
    uint16_t seq;                     /* echoed by the reply */
    uint8_t  type;
    uint8_t  status;                  /* replies only */
    uint8_t  len;
    uint8_t  reserved[3];
    uint8_t  payload[IPC_PAYLOAD_MAX];
} __attribute__((aligned(4))) ipc_msg_t;

typedef struct ipc_ring {
    volatile uint32_t head;           /* producer writes, consumer reads */
    volatile uint32_t tail;           /* consumer writes, producer reads */
    ipc_msg_t slot[IPC_RING_SLOTS];
} ipc_ring_t;

typedef struct ipc_shared {
    volatile uint32_t magic;
    volatile uint32_t version;
    ipc_ring_t req;                   /* CM7 -> CM4 requests */
    ipc_ring_t rsp;                   /* CM4 -> CM7 replies */
    ipc_ring_t evt;                   /* CM4 -> CM7 events */
    ipc_ring_t evt_rsp;               /* CM7 -> CM4 replies to events */
} ipc_shared_t;

extern ipc_shared_t shared_ipc;

/* CM7, before CM4 is released: stamps the header and empties both rings. */
void ipc_init(void);

/* Both cores: the header agrees, so the far side speaks this protocol. */
int ipc_ready(void);

/* CM7. Returns 0 on success and writes the sequence number to match a reply
 * against; non-zero when the ring is full. */
int ipc_send_request(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out);

/* CM7, non-blocking. 1 when a reply for seq was taken, 0 when nothing matched.
 * Replies carrying any other sequence number are dropped: they belong to a
 * request that already gave up. */
int ipc_poll_reply(uint16_t seq, ipc_msg_t *out);

/* CM4. 1 when a request was taken. */
int ipc_poll_request(ipc_msg_t *out);

/* CM4. Answers the request in kind, echoing its sequence number. */
int ipc_send_reply(const ipc_msg_t *req, uint8_t status, const void *payload, uint8_t len);

/* Replies discarded because no caller was waiting on their sequence number.
 * Non-zero means requests are timing out and CM4 is answering late. */
/* CM4. The mirror of ipc_send_request, in the other direction: an event the
 * radio originates and CM7 has to answer. Same sequence-number contract. */
int ipc_send_event(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out);

/* CM4, non-blocking. 1 when a reply for seq was taken. */
int ipc_poll_event_reply(uint16_t seq, ipc_msg_t *out);

/* CM7. 1 when an event was taken. */
int ipc_poll_event(ipc_msg_t *out);

/* CM7. Answers an event in kind, echoing its sequence number. */
int ipc_send_event_reply(const ipc_msg_t *evt, uint8_t status, const void *payload, uint8_t len);

uint32_t ipc_stale_replies(void);

/* Event replies discarded because CM4 had already given up on their sequence
 * number. Counted separately from ipc_stale_replies: a pairing that times out
 * on CM7's arithmetic and a CLI request that times out on the radio are
 * different faults, and one number for both would name neither. */
uint32_t ipc_stale_event_replies(void);

void ipc_ring_state(const ipc_ring_t *r, uint32_t *head, uint32_t *tail);
