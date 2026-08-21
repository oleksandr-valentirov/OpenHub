#pragma once

#include <stdint.h>
#include <stddef.h>

#include "shared_memory.h"

/**
 * @file ipc.h
 * @brief Cross-core mailbox: sequence-numbered SPSC rings in SRAM4.
 *
 * radio_devices_docs/open_hub/arch/ipc.md
 */

#define IPC_MAGIC        0x4F484231u   /* 'OHB1' - both cores must agree */
/* 3 widened the payload, which is exactly what the half-flash gate is for.
 * radio_devices_docs/open_hub/arch/ipc.md */
#define IPC_VERSION      3u
#define IPC_RING_SLOTS   8u            /* power of two */
/* Buffer for a PAIR_INIT; radio_protocol.h owns the frame's real size. */
#define RADIO_PAIR_INIT_MAX  32u
/* Was 64. SRAM4 pays 3 rings x 8 slots for every byte of it.
 * radio_devices_docs/open_hub/arch/ipc.md */
#define IPC_PAYLOAD_MAX  96u

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
    IPC_REQ_QUIESCE,         /**< test scaffolding: force a quiesce with no device */
    IPC_REQ_GET_STORE,
    IPC_REQ_KV_TORN,         /**< test scaffolding: a bad record in the counter log */
    IPC_REQ_INSTALL_DEVICE,  /**< the keys and slot of an already-paired device */
    IPC_REQ_SET_REPORT_RATE, /**< what future pairings grant, in superframes */
    IPC_REQ_GET_EXCHANGE,    /**< the four-frame exchange's own counters */
    IPC_REQ_GET_VECTORS,     /**< which vector sets the radio core was built against */
    IPC_REQ_GET_RXDIAG,      /**< what the radio is hearing, below the frame layer */
    /* Loops a pattern through the radio FIFO and back; nothing on air. */
    IPC_REQ_SPI_LOOP,
    IPC_REQ_HOP_AT,         /**< which channel a given superframe lands on */
    IPC_REQ_GET_DOWNLINK,   /**< the hub's half of the periodic exchange */
    /* Built and MACed by CM7, for CM4 to key at a named superframe. */
    IPC_REQ_SET_PAIR_INIT,
    IPC_REQ_GET_PAIR_INIT,  /**< what CM4 did with the ones it was given */
    IPC_REQ_GET_SYNCTIME,   /**< when the sync word landed, not just that it did */
    IPC_REQ_GET_SYNCSTATS,  /**< the second moment of the same edge, and its regressor */
    IPC_REQ_GET_AFC,        /**< how far off centre each received frame arrived */
    IPC_REQ_GET_AFC_RAW,    /**< the same, one entry per frame, for the scatter */
    IPC_REQ_SET_LNA,        /**< pin the front-end gain, or hand it back to AGC */
    IPC_REQ_GET_EVT_LAT     /**< how long an arrival takes to reach CM7 and back */
};

/* Events, CM4 -> CM7: a separate ring for what the radio originates.
 * radio_devices_docs/open_hub/arch/ipc.md */
enum {
    IPC_EVT_NONE = 0,
    IPC_EVT_PAIR_REQ,        /**< a device answered the join beacon */
    IPC_EVT_PAIR_CONF,       /**< ... and confirmed the derived secret */
    /* An authenticated report, pushed on arrival. ROADMAP item 2 */
    IPC_EVT_UPLINK
};

/* reply status */
enum {
    IPC_ST_OK = 0,
    IPC_ST_UNKNOWN_REQ,
    IPC_ST_BAD_ARG,
    IPC_ST_RADIO_ERR
};

/* Reply for IPC_REQ_GET_TIMING: beacon lateness against the boundary.
 * radio_devices_docs/open_hub/radio/timebase.md */
typedef struct ipc_timing {
    uint32_t superframe;
    uint32_t now_tk;         /**< raw TIM2, so the tick rate is checkable from the console */
    uint32_t late_last_us;
    uint32_t late_max_us;
    uint32_t late_min_us;
    uint32_t period_us;      /**< nominal, what the schedule is written in */
    uint32_t period_tk;      /**< what the grid actually steps, after calibration */
    int32_t  calib_ppm;      /**< timer clock offset from nominal */
    int32_t  calib_ppm_min;  /**< spread across windows; wide means a bad measurement */
    int32_t  calib_ppm_max;
    uint32_t span_lo;        /**< per-capture span extremes, last window */
    uint32_t span_hi;
    uint32_t calib_windows;
    uint32_t calib_rejects;
    uint32_t late_over;      /**< beacons that left later than the cost limit */
    uint32_t calib_age_tk;   /**< ticks since the last accepted window */
} __attribute__((packed)) ipc_timing_t;

/* The radio's pairing state machine, shared so the CLI can name the states. */
typedef enum radio_pair_state {
    RADIO_PAIR_IDLE = 0,   /**< grid running, nothing on the join channel */
    RADIO_PAIR_LISTEN,     /**< window open: join beacon and RX in the join region */
    RADIO_PAIR_QUIESCE     /**< announced on air; grid silent, hub on the join channel */
} radio_pair_state_t;

/* How far the conversation with one device has got, independent of the grid. */
typedef enum radio_exchange_state {
    RADIO_EX_IDLE = 0,
    RADIO_EX_WAIT_RSP,    /**< PAIR_REQ forwarded; CM7 is doing the curve work */
    RADIO_EX_SENT_RSP,    /**< PAIR_RSP is on air; waiting for PAIR_CONF */
    RADIO_EX_WAIT_KEYS,   /**< PAIR_CONF forwarded; CM7 is checking it */
    RADIO_EX_ACCEPTED     /**< PAIR_ACCEPT sent; the device is installed */
} radio_exchange_state_t;

/* Why the last beacon failed, and why the last join frame was dropped.
 * radio_devices_docs/open_hub/arch/ipc.md */
enum {
    RADIO_BERR_NONE = 0,
    RADIO_BERR_BUILD,        /**< payload would not fit the frame buffer */
    RADIO_BERR_PRF,          /**< the hop PRF failed - no channel, so no transmit */
    RADIO_BERR_RETUNE,
    RADIO_BERR_TX
};

enum {
    RADIO_DROP_NONE = 0,
    RADIO_DROP_VERSION,      /**< too short, or a protocol version we do not speak */
    RADIO_DROP_TYPE,         /**< a frame type that has no business on this channel */
    RADIO_DROP_LEN,          /**< right type, wrong length - a contract mismatch */
    RADIO_DROP_IDS,          /**< kept for the wire; the three below replace it */
    RADIO_DROP_NO_WINDOW,    /**< nobody has pressed the button; not a fault */
    RADIO_DROP_BUSY,         /**< an exchange is already in flight */
    RADIO_DROP_NET_ID,
    RADIO_DROP_HUB_ID,
    RADIO_DROP_DEV_ID
};
#define RADIO_DROP_COUNT 10u

/* Reply payload for IPC_REQ_GET_PAIR_STATE. */
typedef struct ipc_pair_state {
    uint8_t  state;          /**< radio_pair_state_t */
    uint8_t  quiesce_left;   /**< superframes until the grid resumes */
    uint8_t  beacon_err_last;  /**< RADIO_BERR_* */
    uint8_t  reqs_drop_last;   /**< RADIO_DROP_* */
    uint32_t dev_id;         /**< the device the window was opened for */
    uint32_t window_left_ms;
    uint32_t resume_at;      /**< superframe the hub has committed to resuming on */
    uint32_t reqs_seen;      /**< PAIR_REQ frames received on the join channel */
    uint32_t reqs_dropped;   /**< ... and rejected: wrong id, bad length, no window */
    uint32_t join_regions;   /**< join regions entered */
    uint32_t join_beacons;   /**< join beacons handed to the radio */
    uint32_t join_tx_err;    /**< ... of which the driver refused or timed out */
    uint32_t data_beacons;      /**< beacon attempts, so the accounting stays exact */
    uint32_t announce_beacons;
    uint32_t silent_frames;
    uint32_t quiesce_refused;   /**< asked for one inside the minimum gap */
    uint32_t beacon_err;        /**< beacons the radio would not put on air */
    uint32_t quiesce_lost;      /**< valid PAIR_REQs that won no clear air */
} __attribute__((packed)) ipc_pair_state_t;

/* Reply for IPC_REQ_GET_STORE: the durable superframe ceiling and its margin. */
typedef struct ipc_store_state {
    uint32_t reserved;    /**< nothing at or below this may be reused */
    uint32_t counter;     /**< where the counter is now, so the margin is visible */
    uint32_t writes;
    uint32_t errors;      /**< non-zero means the ceiling has stopped advancing */
    uint32_t slots_left;
    uint32_t unreserved;  /**< superframes the radio stayed silent to avoid reuse */
} __attribute__((packed)) ipc_store_state_t;

/* A reply that outgrows its slot is truncated on the wire, not refused here.
 * radio_devices_docs/open_hub/arch/ipc.md */
_Static_assert(sizeof(ipc_timing_t)     <= IPC_PAYLOAD_MAX, "ipc_timing_t too large");
_Static_assert(sizeof(ipc_pair_state_t) <= IPC_PAYLOAD_MAX, "ipc_pair_state_t too large");
_Static_assert(sizeof(ipc_store_state_t) <= IPC_PAYLOAD_MAX, "ipc_store_state_t too large");
/* IPC_EVT_PAIR_REQ: what CM4 pulled out of an already-checked PAIR_REQ. */
typedef struct ipc_pair_req_evt {
    uint32_t dev_id;
    uint32_t superframe;     /**< the counter the device echoed back */
    uint8_t  dev_nonce[8];   /**< the device's contribution of freshness */
    uint8_t  pubkey[33];     /**< the device's static key, compressed SEC1 */
} __attribute__((packed)) ipc_pair_req_evt_t;

/* ... and its reply: the body of PAIR_RSP, which CM4 frames and transmits. */
typedef struct ipc_pair_rsp_evt {
    uint8_t eph_pubkey[33];
    uint8_t confirm[16];
} __attribute__((packed)) ipc_pair_rsp_evt_t;

/* IPC_EVT_PAIR_CONF: the device's confirmation, checked against CM7's transcript. */
typedef struct ipc_pair_conf_evt {
    uint32_t dev_id;
    uint8_t  confirm[16];
} __attribute__((packed)) ipc_pair_conf_evt_t;

/* Everything CM4 needs to serve one paired device: pairing reply and install alike.
 * radio_devices_docs/radio/crypto/key-lifecycle.md */
typedef struct ipc_device_keys {
    uint32_t dev_id;
    uint32_t key_gen;
    uint8_t  session_key[16];
    uint8_t  hop_key[16];    /**< the network key, identical for every device */
    uint8_t  slot;
    uint8_t  report_every;
    uint8_t  flags;
    uint8_t  reserved;
} __attribute__((packed)) ipc_device_keys_t;

/* Reply for IPC_REQ_GET_EXCHANGE: the four-frame exchange's own counters. */
typedef struct ipc_exchange_state {
    uint8_t  state;            /**< radio_exchange_state_t */
    uint8_t  aead_selftest;    /**< 0 = passed; otherwise the failing check */
    uint8_t  devices;          /**< installed on the radio core */
    uint8_t  report_every;     /**< granted to the next pairing */
    uint32_t dev_id;           /**< the exchange in flight, if any */
    uint32_t reqs_forwarded;   /**< PAIR_REQ handed to CM7 */
    uint32_t rsp_sent;
    uint32_t confs_forwarded;
    uint32_t accepts_sent;
    uint32_t paired;
    uint16_t cm7_refused;      /**< CM7 answered, and said no; 16-bit to fit the slot */
    uint16_t timeouts;         /**< CM7 or the device never answered */
    uint16_t tx_err;
    uint16_t seal_err;
    uint32_t uplink_frames;    /**< frames received in the uplink region */
    uint32_t uplink_ok;        /**< ... whose tag verified */
    uint16_t uplink_bad_slot;  /**< ... naming a slot with no device */
    uint16_t uplink_bad_frame; /**< wrong length, type or version */
    uint16_t uplink_bad_tag;   /**< well formed, and it did not authenticate */
    uint16_t uplink_replay;    /**< authenticated, and not newer than the floor */
    uint32_t uplink_windows;   /**< receive windows actually opened */
    uint32_t uplink_sync;      /**< ... and sync detections inside them */
    uint32_t uplink_evt_drop;  /**< arrivals CM7 was never told about */
} __attribute__((packed)) ipc_exchange_state_t;

/* Reply for IPC_REQ_GET_RXDIAG: what the radio hears, below the frame layer.
 * radio_devices_docs/open_hub/radio/configuration.md */
typedef struct ipc_rx_diag {
    uint32_t sync_match;    /**< rising edges of SyncAddressMatch */
    uint32_t crc_err;       /**< delivered by the part, failed its CRC */
    uint32_t frames;        /**< delivered with the CRC verified */
    uint32_t last_superframe;
    uint8_t  last_len;      /**< recorded before anything is judged about it */
    uint8_t  last_type;
    int8_t   last_rssi;
    int8_t   rssi_peak;     /**< RegRssiValue in the join window, below the sync word */
    int8_t   rssi_floor;
    int8_t   up_rssi_peak;  /**< the same, taken inside the uplink slot region */
    int8_t   up_rssi_floor;
    uint8_t  reserved[1];
    uint32_t flushes;       /**< receivers restarted because the FIFO could not drain */
    uint32_t rssi_samples;  /**< measurements taken, so peak 0 stays readable */
    uint32_t drop_hub_id;   /**< the identifiers the last refused PAIR_REQ carried */
    uint32_t drop_dev_id;
    uint16_t drop_net_id;
    uint8_t  reserved2[2];
    uint8_t  drop_head[16]; /**< the last PAIR_REQ off the FIFO, undecoded */
    uint8_t  drop_key[8];   /**< frame offset 24..31, the public key's head */
} __attribute__((packed)) ipc_rx_diag_t;

/* Request for IPC_REQ_SET_PAIR_INIT: a frame CM4 keys without parsing. ADR-0021 */
typedef struct ipc_pair_init {
    uint32_t superframe;     /**< key it when the counter reads exactly this */
    uint8_t  len;
    uint8_t  frame[RADIO_PAIR_INIT_MAX];
} __attribute__((packed)) ipc_pair_init_t;
_Static_assert(sizeof(ipc_pair_init_t) <= IPC_PAYLOAD_MAX, "ipc_pair_init_t too large");

/* Reply for IPC_REQ_GET_PAIR_INIT: every way one can fail to reach the air. */
typedef struct ipc_pair_init_state {
    uint32_t given;          /**< frames accepted from CM7 */
    uint32_t sent;
    uint32_t missed;         /**< its superframe passed before the region opened */
    uint32_t tx_err;
    uint32_t replaced;       /**< a new frame arrived while one was still pending */
    uint32_t last_sent_sf;
    uint32_t pending_sf;     /**< 0 when nothing is queued */
    uint32_t frf;            /**< read back off the part after the transmit */
    uint8_t  payload_len;
} __attribute__((packed)) ipc_pair_init_state_t;
_Static_assert(sizeof(ipc_pair_init_state_t) <= IPC_PAYLOAD_MAX,
               "ipc_pair_init_state_t too large");

/* Reply for IPC_REQ_GET_SYNCTIME: where a frame's sync word landed.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
typedef struct ipc_synctime {
    uint32_t edges;          /**< rising edges of SyncAddressMatch, from the ISR */
    uint32_t frames;         /**< accepted frames, from the same read */
    uint32_t implausible;    /**< stamped outside the superframe it belongs to */
    uint32_t last_offset_us; /**< of the last edge, from the superframe boundary */
    uint32_t min_offset_us;
    uint32_t max_offset_us;
    uint32_t last_superframe;
    uint8_t  dio_map1;       /**< read back off the part, not the value written */
    uint8_t  dio3_asked;     /**< what the mapping was set to */
    uint8_t  reserved[2];
    uint32_t lead_last_us;   /**< command instant to first bit: PacketSent minus air */
    uint32_t lead_min_us;
    uint32_t lead_max_us;
    uint32_t lead_n;         /**< frames the lead was measured over */
    uint32_t last_offset_tk; /**< the raw tick delta, before the scale was applied */
    int32_t  calib_ppm;      /**< the scale in force when it was converted */
} __attribute__((packed)) ipc_synctime_t;

_Static_assert(sizeof(ipc_synctime_t) <= IPC_PAYLOAD_MAX, "ipc_synctime_t too large");

/* Reply for IPC_REQ_GET_SYNCSTATS: sums, so a spread is reported, not a range.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */

/* Every sum shares one n; an edge with no beacon is counted in unpaired. */
typedef struct ipc_syncstats {
    uint32_t ref_us;         /**< deviations are from the first sample, not from zero */
    uint32_t n;              /**< the one population all five sums share */
    int64_t  sum_d;          /**< of (offset - ref_us) */
    uint64_t sumsq_d;
    uint64_t lead_sum;       /**< of the beacon lead that preceded each of those n */
    uint64_t lead_sumsq;
    int64_t  cov_sum;        /**< of (offset - ref_us) * lead */
    uint32_t unpaired;       /**< edges dropped for having no beacon to pair with */
    uint32_t beacon_n;       /**< every beacon, which is a wider population than n */
    uint32_t beacon_min_us;
    uint32_t beacon_max_us;
} __attribute__((packed)) ipc_syncstats_t;

_Static_assert(sizeof(ipc_syncstats_t) <= IPC_PAYLOAD_MAX, "ipc_syncstats_t too large");

/* Reply for IPC_REQ_GET_AFC: the correction AFC applied to each frame received.
 * radio_devices_docs/open_hub/radio/configuration.md */
typedef struct ipc_afc {
    uint32_t n;          /**< frames measured, so a correction of 0 Hz stays readable */
    uint32_t read_err;   /**< SPI reads that failed, never counted as a sample */
    int32_t  last_hz;
    int32_t  min_hz;
    int32_t  max_hz;
    uint8_t  last_grid;  /**< the channel that sample was taken on */
    uint8_t  reserved[3];
    int64_t  sum_hz;     /**< the four sums share n: a fit of the error against grid */
    int64_t  sum_g;
    int64_t  sum_gg;
    int64_t  sum_gh;
} __attribute__((packed)) ipc_afc_t;

_Static_assert(sizeof(ipc_afc_t) <= IPC_PAYLOAD_MAX, "ipc_afc_t too large");

/* Parallel arrays, not an array of pairs: an inner struct does not inherit packed.
 * radio_devices_docs/open_hub/radio/configuration.md */
#define IPC_AFC_RING   9u

/* Newest first. Three frames arrive 611 ms apart and no poll rate separates them. */
typedef struct ipc_afc_raw {
    uint32_t total;              /**< samples taken, so a wrapped ring is visible */
    uint8_t  n;                  /**< entries below that are filled */
    uint8_t  reserved;
    uint16_t crc_ok;             /**< bit i: entry i passed its CRC */
    uint16_t in_frame;           /**< bit i: entry i's level was sampled inside the frame */
    uint16_t lag_max_us;         /**< worst gap from a sync edge to its level sample */
    uint16_t rssi_taken;         /**< level samples triggered, saturating at 65535 */
    uint16_t rssi_late;          /**< ... of those, the ones the frame had outrun */
    uint16_t rssi_err;           /**< ... and the ones the register read refused */
    uint8_t  grid[IPC_AFC_RING];
    uint8_t  slot[IPC_AFC_RING]; /**< the opportunity it arrived in, 0xFF unplaceable */
    uint8_t  gain[IPC_AFC_RING]; /**< LnaCurrentGain in force on that frame */
    int8_t   rssi[IPC_AFC_RING]; /**< dBm at sync match, not after the frame ended */
    int16_t  afc[IPC_AFC_RING];  /**< the AFC register in Fstep, hertz is the reader's */
} __attribute__((packed)) ipc_afc_raw_t;

_Static_assert(IPC_AFC_RING <= 16u,
               "crc_ok and in_frame are uint16 bitmasks, one bit per entry");

/* Fstep units; `device afc` prints the driver's hertz for the same frame.
 * radio_devices_docs/open_hub/radio/configuration.md */
#define IPC_AFC_STEPS_TO_HZ(steps)  ((int32_t)(((int64_t)(steps) * 32000000) >> 19))

_Static_assert(IPC_AFC_STEPS_TO_HZ(16384) == 1000000,
               "Fstep is FXOSC/2^19, so 16384 steps is exactly one megahertz");

_Static_assert(sizeof(ipc_afc_raw_t) <= IPC_PAYLOAD_MAX, "ipc_afc_raw_t too large");

/* Reply for IPC_REQ_GET_EVT_LAT: both terms on CM4's clock. ROADMAP item 2
 * radio_devices_docs/open_hub/arch/ipc.md */
typedef struct ipc_evt_latency {
    uint32_t sent;              /**< uplink events handed to the ring */
    uint32_t replied;           /**< ... whose reply came back and was timed */
    uint32_t lost;              /**< ... superseded before their reply arrived */
    uint32_t arrival_last_us;   /**< frame end to the event leaving: this core's work */
    uint32_t arrival_max_us;
    uint32_t arrival_bad;       /**< edges that fired sooner than the air allows */
    uint32_t rtt_last_us;       /**< the event leaving to its reply: bounds CM7's half */
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint64_t rtt_sum_us;        /**< a sum, so the reader divides by a stated n */
    uint32_t stale;             /**< replies nobody was waiting for, never silent */
} __attribute__((packed)) ipc_evt_latency_t;

_Static_assert(sizeof(ipc_evt_latency_t) <= IPC_PAYLOAD_MAX,
               "ipc_evt_latency_t too large");


/* Reply for IPC_REQ_HOP_AT. The key travels with the answer, so two sides can
 * compare. radio_devices_docs/radio/hopping.md */
typedef struct ipc_hop_at {
    uint32_t superframe;
    uint8_t  channel;        /**< index into the hop set */
    uint8_t  grid_slot;      /**< ... mapped past the reserved join slot */
    uint8_t  placeholder;    /**< 1 while CM7 has not installed the network key */
    uint8_t  key_head[4];
    uint32_t hz;             /**< the carrier that slot maps to */
    uint8_t  count;          /**< the hop set this deck was built over */
    uint8_t  deck[32];       /**< the permutation itself, not a channel from it */
} __attribute__((packed)) ipc_hop_at_t;
_Static_assert(sizeof(ipc_hop_at_t) <= IPC_PAYLOAD_MAX, "ipc_hop_at_t too large");

/* Reply payload for IPC_REQ_GET_DOWNLINK. */
typedef struct ipc_downlink_state {
    uint32_t opportunities;  /**< superframes whose parity said a downlink is due */
    uint32_t sent;
    uint32_t seal_err;
    uint32_t tx_err;
    uint32_t no_device;      /**< the rotation found nobody, with devices installed */
    uint32_t prf_err;        /**< no hop channel, so nothing was transmitted */
    uint32_t last_hz;        /**< the carrier the last downlink actually left on */
    uint32_t last_superframe;
    uint8_t  next_slot;      /**< where the round robin resumes */
    uint8_t  reserved[3];
    uint32_t cmd_sent;       /**< commands that reached the air, not devices */
    uint32_t cmd_replaced;   /**< queued over one that had not flown yet */
    uint32_t cmd_acked;      /**< the device echoed the seq back */
    uint32_t cmd_lost;       /**< every repeat spent and no echo ever came */
} __attribute__((packed)) ipc_downlink_state_t;
_Static_assert(sizeof(ipc_downlink_state_t) <= IPC_PAYLOAD_MAX,
               "ipc_downlink_state_t too large");

/* Reply payload for IPC_REQ_SPI_LOOP. */
typedef struct ipc_spi_loop {
    uint32_t reg_bad_bytes;  /**< the register arm: the control for the FIFO arm below */
    uint32_t reg_xor_80;
    uint8_t  reg_read[8];
    uint32_t passes;
    uint32_t bad_bytes;      /**< bytes that came back different */
    uint32_t bad_passes;     /**< passes with at least one */
    uint32_t xor_80;         /**< ... of which the difference was bit 7 alone */
    uint8_t  first_bad[8];   /**< what came back, from the first failing pass */
    uint8_t  expect[8];      /**< and what was written */
    uint32_t io_err;
    uint32_t spi_hz;         /**< read out of the peripheral, not from the prescaler */
} __attribute__((packed)) ipc_spi_loop_t;
_Static_assert(sizeof(ipc_spi_loop_t) <= IPC_PAYLOAD_MAX, "ipc_spi_loop_t too large");

/* Reply for IPC_REQ_GET_VECTORS: a consumer of the digests that can fail.
 * radio_devices_docs/open_hub/arch/build-and-generation.md */
typedef struct ipc_vectors {
    char    pair[17];        /**< PAIR_VECTORS_DIGEST, NUL-terminated */
    char    hop[17];         /**< HOP_VECTORS_DIGEST */
    uint8_t pair_version;
    uint8_t reserved[3];
} __attribute__((packed)) ipc_vectors_t;

/* Reply for IPC_REQ_GET_DEVICE_INFO, which asks for the index-th live device. */
typedef struct ipc_device_report {
    uint32_t dev_id;
    uint32_t last_superframe;  /**< of the last report whose tag verified */
    uint32_t frames_ok;
    uint32_t frames_bad;       /**< wrong length, wrong version, or a failed tag */
    uint32_t uptime_s;         /**< as the device reported it */
    uint8_t  total;            /**< live devices, so one round trip sizes the list */
    uint8_t  slot;
    int8_t   rssi_up;          /**< dBm off the RSSI latch, which nothing here triggers. ROADMAP item 14 */
    int8_t   rssi_down;        /**< dBm, as the device heard the hub's last beacon */
    uint16_t supply_mv;
    uint8_t  report_every;
    uint8_t  flags;            /**< RADIO_REPORT_FLAG_* from the last report */
    uint8_t  ack_arg;          /**< the argument the device said it applied, per ack_cmd */
    uint8_t  reserved;
    uint32_t arrival_us;       /**< into its superframe, so the air half is checkable */
    uint8_t  cmd_every;        /**< report_every the last SET_RATE carried, 0 if none */
    uint8_t  cmd_state;        /**< 0 none, 1 still riding downlinks, 2 acked */
    uint16_t cyc_min;          /**< shortest gap between cycles: the observed cadence */
    uint16_t cyc_n;            /**< gaps measured, so the mean has a stated n */
    uint32_t cyc_sum;          /**< ... their sum; the mean carries delivery loss too */
} __attribute__((packed)) ipc_device_report_t;

_Static_assert(sizeof(ipc_pair_req_evt_t)  <= IPC_PAYLOAD_MAX, "ipc_pair_req_evt_t too large");
_Static_assert(sizeof(ipc_pair_rsp_evt_t)  <= IPC_PAYLOAD_MAX, "ipc_pair_rsp_evt_t too large");
_Static_assert(sizeof(ipc_pair_conf_evt_t) <= IPC_PAYLOAD_MAX, "ipc_pair_conf_evt_t too large");
/* Request for IPC_REQ_SET_DEVICE_PARAM: one command queued for one device.
 * radio_devices_docs/radio/tdma.md */
typedef struct ipc_device_cmd {
    uint32_t dev_id;
    uint8_t  cmd;            /**< RADIO_CMD_* */
    uint8_t  report_every;   /**< SET_RATE only; 0 leaves the grant alone */
    uint16_t arg;
    uint8_t  repeats;        /**< downlinks it rides: nothing acknowledges it */
} __attribute__((packed)) ipc_device_cmd_t;

_Static_assert(sizeof(ipc_device_cmd_t) <= IPC_PAYLOAD_MAX, "ipc_device_cmd_t too large");
_Static_assert(sizeof(ipc_device_keys_t)   <= IPC_PAYLOAD_MAX, "ipc_device_keys_t too large");
_Static_assert(sizeof(ipc_device_report_t) <= IPC_PAYLOAD_MAX, "ipc_device_report_t too large");
_Static_assert(sizeof(ipc_exchange_state_t) <= IPC_PAYLOAD_MAX, "ipc_exchange_state_t too large");
_Static_assert(sizeof(ipc_vectors_t) <= IPC_PAYLOAD_MAX, "ipc_vectors_t too large");
_Static_assert(sizeof(ipc_rx_diag_t) <= IPC_PAYLOAD_MAX, "ipc_rx_diag_t too large");

typedef struct ipc_msg {
    uint16_t seq;                     /**< echoed by the reply */
    uint8_t  type;
    uint8_t  status;                  /**< replies only */
    uint8_t  len;
    uint8_t  reserved[3];
    uint8_t  payload[IPC_PAYLOAD_MAX];
} __attribute__((aligned(4))) ipc_msg_t;

typedef struct ipc_ring {
    volatile uint32_t head;           /**< producer writes, consumer reads */
    volatile uint32_t tail;           /**< consumer writes, producer reads */
    ipc_msg_t slot[IPC_RING_SLOTS];
} ipc_ring_t;

typedef struct ipc_shared {
    volatile uint32_t magic;
    volatile uint32_t version;
    ipc_ring_t req;                   /**< CM7 -> CM4 requests */
    ipc_ring_t rsp;                   /**< CM4 -> CM7 replies */
    ipc_ring_t evt;                   /**< CM4 -> CM7 events */
    ipc_ring_t evt_rsp;               /**< CM7 -> CM4 replies to events */
} ipc_shared_t;

extern ipc_shared_t shared_ipc;

/**
 * @brief Stamps the header and empties both rings, on CM7 before CM4 is released.
 *
 * (NOLOAD) means the region holds whatever the last boot left, so CM4 must never
 * read a ring before this runs. radio_devices_docs/open_hub/arch/ipc.md
 */
void ipc_init(void);

/**
 * @brief Whether the far side speaks this protocol.
 * @retval 1  magic and version both agree
 * @retval 0  the mailbox is disabled rather than falling back to an older layout
 */
int ipc_ready(void);

/**
 * @brief Pushes one request, CM7 to CM4.
 * @param type     the IPC_REQ_* being asked for
 * @param payload  the body, or NULL
 * @param len      its length, at most IPC_PAYLOAD_MAX
 * @param seq_out  receives the sequence number a reply must echo
 * @retval  0  queued
 * @retval !=0 the ring was full; nothing is retried
 */
int ipc_send_request(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out);

/**
 * @brief Takes the reply matching one sequence number, on CM7, without blocking.
 * @param seq  the number ipc_send_request() gave out
 * @param out  receives the reply
 * @retval 1  a matching reply was taken
 * @retval 0  none yet
 *
 * Anything not matching is discarded, so two concurrent pollers eat each other's
 * answers. That is why hub_ipc_call() holds its mutex across the whole transaction.
 */
int ipc_poll_reply(uint16_t seq, ipc_msg_t *out);

/**
 * @brief Takes one request, on CM4.
 * @param out  receives it
 * @retval 1  a request was taken
 * @retval 0  the ring is empty
 */
int ipc_poll_request(ipc_msg_t *out);

/**
 * @brief Answers a request in kind, echoing its sequence number.
 * @param req      the request being answered
 * @param status   IPC_ST_*, including IPC_ST_UNKNOWN_REQ
 * @param payload  the reply body, or NULL
 * @param len      its length
 * @retval  0  queued
 * @retval !=0 the ring was full
 *
 * Every request is answered, including types that fall through the switch: a
 * waiter must never block because CM4 did not recognise it.
 */
int ipc_send_reply(const ipc_msg_t *req, uint8_t status, const void *payload, uint8_t len);

/**
 * @brief Pushes an event CM7 has to answer, the mirror of ipc_send_request().
 * @param type     the IPC_EVT_* the radio originated
 * @param payload  the body, or NULL
 * @param len      its length
 * @param seq_out  receives the sequence number the reply will echo
 * @retval  0  queued
 * @retval !=0 the ring was full
 */
int ipc_send_event(uint8_t type, const void *payload, uint8_t len, uint16_t *seq_out);

/**
 * @brief Takes the event reply matching one sequence number, on CM4.
 * @param seq  the number ipc_send_event() gave out
 * @param out  receives the reply
 * @retval 1  a matching reply was taken
 * @retval 0  none yet
 *
 * Filtering: everything else is discarded on the way past. With more than one
 * waiter this silently eats the other's reply - use ipc_poll_any_event_reply().
 */
int ipc_poll_event_reply(uint16_t seq, ipc_msg_t *out);

/**
 * @brief Takes any event reply, so one dispatcher can serve every waiter.
 * @param out  receives the reply, whatever it answers
 * @retval 1  a reply was taken
 * @retval 0  the ring is empty
 *
 * CM4 has no mutex and no threads, so the one-caller rule is kept by there being
 * one reader. radio_devices_docs/open_hub/arch/ipc.md
 */
int ipc_poll_any_event_reply(ipc_msg_t *out);

/**
 * @brief Takes one event, on CM7.
 * @param out  receives it
 * @retval 1  an event was taken
 * @retval 0  the ring is empty
 */
int ipc_poll_event(ipc_msg_t *out);

/**
 * @brief Answers an event in kind, echoing its sequence number.
 * @param evt      the event being answered
 * @param status   IPC_ST_*
 * @param payload  the reply body, or NULL
 * @param len      its length
 * @retval  0  queued
 * @retval !=0 the ring was full
 */
int ipc_send_event_reply(const ipc_msg_t *evt, uint8_t status, const void *payload, uint8_t len);

/**
 * @brief Replies discarded because nobody was waiting on their sequence number.
 * @return count; non-zero means requests are timing out and CM4 is answering late
 */
uint32_t ipc_stale_replies(void);

/**
 * @brief The same for events, counted apart.
 * @return count; a pairing timing out on CM7 and a CLI request timing out on the
 *         radio are different faults, and one number for both names neither
 */
uint32_t ipc_stale_event_replies(void);

/**
 * @brief Reads a ring's indices, for the console to print occupancy.
 * @param r     the ring
 * @param head  receives the producer's index
 * @param tail  receives the consumer's index
 */
void ipc_ring_state(const ipc_ring_t *r, uint32_t *head, uint32_t *tail);
