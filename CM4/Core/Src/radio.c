/**
 * @file radio.c
 * @brief CM4's radio: the superframe grid, every frame on it, and the pairing window.
 *
 * radio_devices_docs/open_hub/radio/superloop.md
 */
#include <stddef.h>
#include <string.h>

#include "radio.h"
#include "rfm69.h"
#include "rfm69_registers.h"
#include "phy.h"
#include "phy_rfm69.h"
#include "timebase.h"
#include "hsem_table.h"
#include "shared_memory.h"
#include "ipc.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "radio_phy.h"
#include "hop.h"
#include "hop_v1.h"
#include "pair_v4.h"
#include "calib.h"
#include "bootwait.h"
#include "build_id.h"

/* The slack a throwaway build narrows so the guard must refuse.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
#ifdef RADIO_SYNC_PAIR_SLACK_US
#define BUILD_SUFFIX  "+narrow"
#else
#define RADIO_SYNC_PAIR_SLACK_US  RADIO_SLOT_US
#define BUILD_SUFFIX  ""
#endif
/* Truncation would emit a plausible id; the build fails instead. */
_Static_assert(sizeof(BUILD_ID BUILD_SUFFIX) <= 24u, "build id does not fit");
#include "kvstore.h"
#include "aead.h"
#include "main.h"

/* The PHY numbers live in radio_phy.h, where both firmwares compile the same ones.
 * radio_devices_docs/radio/phy.md */

/* A joining device parks on the fixed channel, so the window bounds airtime only.
 * radio_devices_docs/radio/joining.md */
#define PAIRING_WINDOW_MS       RADIO_PAIR_WINDOW_MS
#define JOIN_BEACON_EVERY       2u


#define MODE_TIMEOUT_US         10000u
/* 2^(smoothing+1) bit periods. Bounds an SPI fault, not the part. */
#define RSSI_TIMEOUT_US         500u
#define TX_TIMEOUT_US           200000u

/* Device indices, counted from 0; the cap is the grid's, not this file's.
 * radio_devices_docs/radio/tdma.md */
#define RADIO_MAX_DEVICES  RADIO_DEVICE_MAX

/* RegDioMapping1 DIO3 field, packet mode: SyncAddress. Unconfirmed on this part. */

/* A staged turn waits a superframe; this outlasts it. ADR-0026 */
#define EX_REGION_TIMEOUT_US  (SUPERFRAME_US + RADIO_JOIN_REGION_US)

/* How long a half-finished exchange may hold the machine.
 * radio_devices_docs/radio/pairing.md */

#define EX_CM7_TIMEOUT_US   2000000u
#define EX_DEV_TIMEOUT_US   3000000u

typedef struct dev_entry {
    uint8_t  used;
    uint8_t  slot;
    uint8_t  report_every;
    uint8_t  flags;             /**< RADIO_REPORT_FLAG_* from the last report */
    uint32_t dev_id;
    uint32_t key_gen;
    uint8_t  session_key[AEAD_KEY_BYTES];
    uint32_t last_superframe;
    uint32_t rx_floor;          /**< highest accepted, scoped to key_gen; the replay guard */
    uint8_t  rx_floor_slot;     /**< ... and which of its three slots, so k=3 is orderable */
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t frames_replay;
    uint32_t uptime_s;
    uint16_t supply_mv;
    int16_t  temp_c_x10;        /**< as the device measured its own die */
    int8_t   rssi_up;           /**< off the RSSI latch, which nothing here triggers. ROADMAP item 14 */
    uint32_t arrival_us;        /**< into the superframe the report claimed */
    uint32_t arrival_sync_us;   /**< the same off the DIO3 edge, or IPC_ARRIVAL_SYNC_NONE */
    uint16_t sync_unpaired;     /**< this device's share of the hub-wide refusals */
    int8_t   rssi_down;         /**< as the device heard the hub's last beacon */
    uint8_t  dl_cmd;            /**< RADIO_CMD_*, queued for this device */
    uint8_t  dl_report_every;
    uint16_t dl_arg;
    uint8_t  dl_repeats;        /**< downlinks left to carry it; 0 means idle */
    uint8_t  dl_cmd_seq;        /**< names the command, so an ack can refer to it */
    uint8_t  dl_acked;          /**< the device echoed this seq back */
    uint8_t  dl_ack_arg;        /**< ... and what it said it applied, never what was asked */
    uint32_t dl_nonce_sf;       /**< the superframe of the last downlink sealed for it */
    uint8_t  dl_nonce_used;     /**< ... and whether there was one, since 0 is a real one */
    uint16_t missed_run;        /**< report opportunities closed in a row with nothing */
    uint32_t cyc_last_sf;       /**< superframe of the last cycle that arrived */
    uint16_t cyc_min;           /**< its shortest gap: what the device's cadence is */
    uint16_t cyc_n;
    uint32_t cyc_sum;           /**< ... against the mean, which also carries loss */
} dev_entry_t;

static void RFM_send_broadcast(uint8_t flags, uint8_t resume_in);
static void RFM_send_join_beacon(void);
static uint8_t RFM_open_pairing(uint32_t dev_id);
static void RFM_serve_request(const ipc_msg_t *req);
static int superframe_due(void);
static void on_superframe(void);
static void join_region_service(void);
static uint8_t begin_quiesce(uint8_t superframes);
static int  join_window_holds(uint8_t payload_b);
static void handle_join_frame(const phy_ev_t *ev);
static int  frame_selftest(void);
static void ex_reset(void);
static void exchange_service(void);
static void uplink_service(void);
static void downlink_service(void);
static int  install_device(const ipc_device_keys_t *k);
static int  remove_device(uint32_t dev_id);


static uint32_t hub_id = 0x33442211u;
static uint32_t frame_counter = 0;
static uint32_t superframe_start_tk = 0;
static uint32_t superframe_tk = 0;   /* SUPERFRAME_US in real ticks */
static uint8_t  grid_started = 0;
static uint32_t late_last_us = 0;
static uint32_t late_max_us = 0;
static uint32_t late_min_us = 0xFFFFFFFFu;
static uint32_t late_over = 0;   /* beacons past RADIO_BEACON_LATE_LIMIT_US */

/* Command instant to first bit on air: FIFO write, PLL lock and PA ramp.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static uint32_t lead_last_us, lead_min_us = 0xFFFFFFFFu, lead_max_us, lead_n;
/* The beacon alone; lead_* above mixes in downlinks and join beacons. */
static uint32_t bl_last_us, bl_min_us = 0xFFFFFFFFu, bl_max_us, bl_n, bl_sf;
static uint32_t pairing_deadline_us = 0;
static uint8_t  pairing_open = 0;
static uint32_t pairing_dev_id = 0;

/* Superframe offsets in real ticks, recomputed at each boundary from one scale.
 * radio_devices_docs/open_hub/radio/timebase.md */
static uint32_t join_offset_tk = 0;

static radio_pair_state_t pair_state = RADIO_PAIR_IDLE;
static uint8_t  quiesce_len = 0;         /* superframes announced */
static uint32_t quiesce_resume_at = 0;   /* the counter value promised on air */
static uint8_t  quiesce_pending = 0;     /* announce at the next boundary */
/* One full gap in the past, so the first quiesce is not refused. */
static uint32_t quiesce_last_end = (uint32_t)(0u - RADIO_QUIESCE_MIN_GAP);
static uint32_t quiesce_refused = 0;
static uint32_t pair_reqs_seen = 0;
static uint32_t pair_reqs_dropped = 0;
static uint32_t join_regions = 0;
static uint32_t join_beacons = 0;
static uint32_t join_tx_err = 0;
static uint32_t data_beacons = 0;
static uint32_t announce_beacons = 0;
static uint32_t silent_frames = 0;
static uint32_t unreserved_frames = 0;   /* boundaries passed with nothing sent */
/* data_beacons counts attempts, which is what keeps the accounting exact.
 * radio_devices_docs/open_hub/arch/ipc.md */
static uint32_t beacon_err = 0;
static uint32_t quiesce_lost = 0;        /* a PAIR_REQ served without clear air */

/* Join-region sub-state, so a 100 ms window never blocks the loop.
 * radio_devices_docs/open_hub/radio/superloop.md */
static uint8_t  join_phase = 0;
static uint8_t  join_beacon_pending = 0;
static uint32_t join_rx_deadline = 0;
static uint32_t join_served_frame = 0xFFFFFFFFu;

/* `sync 0` cannot separate silent air from a receiver that never opened.
 * radio_devices_docs/radio/pairing.md */
static uint32_t jp_windows, jp_passes, jp_probes, jp_not_rx;
static uint32_t jp_inv_probes, jp_inv_not_rx, jp_levels, jp_level_tries;
static int16_t  jp_inv_peak_x2 = -32768, jp_inv_floor_x2 = 32767;
static int16_t  jp_idle_peak_x2 = -32768, jp_idle_floor_x2 = 32767;
static uint8_t  jp_last_op;
static uint8_t  jp_step;        /* which of the two probes this window has taken */

/* Just before a request's preamble, then inside its payload; both off the
 * schedule. radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md */
#define JP_MODE_US   (RADIO_AIR_START_TO_END_US(RADIO_PAIR_INIT_BYTES) + \
                      RADIO_PAIR_REQ_LEAD_US)
_Static_assert(JP_MODE_US < RADIO_TURN_INVITE_US, "the level span must follow the mode probe");
_Static_assert(RADIO_TURN_INVITE_US < RADIO_JOIN_RX_US, "a probe outside the window measures nothing");


/* Indexed by slot, so an uplink frame's slot byte is the whole lookup. */
static dev_entry_t devices[RADIO_MAX_DEVICES];
static uint8_t  device_count;
static uint32_t up_evt_sent, up_evt_drop;   /* ROADMAP item 2 */
/* The hub half of the event deadline, both terms on this core's clock.
 * ROADMAP item 2, radio_devices_docs/open_hub/arch/ipc.md */
static uint16_t evt_seq;            /* the event still waiting for its reply */
static uint8_t  evt_waiting;
static uint32_t evt_sent_tk;        /* when it was handed to the ring */
static uint32_t evt_replied, evt_lost, evt_stale, evt_arrival_bad;
static ipc_msg_t ex_reply;          /* the exchange's answer, held for one pass */
static uint8_t   ex_reply_new;
static uint32_t evt_arrival_last_us, evt_arrival_max_us;
static uint32_t evt_rtt_last_us, evt_rtt_min_us, evt_rtt_max_us;
static uint64_t evt_rtt_sum_us;
/* Sent, acked, lost: an echo names the command, so silence is countable. */
static uint32_t dl_cmd_sent, dl_cmd_replaced, dl_cmd_acked, dl_cmd_lost;
static uint8_t  net_hop_key_set;
static uint8_t  report_every_grant = RADIO_REPORT_EVERY_DEFAULT;
static int      aead_selftest_rc = 1;   /* until it has actually run */

static radio_exchange_state_t ex_state = RADIO_EX_IDLE;
static uint16_t ex_seq;
static uint32_t ex_dev_id;
static uint32_t ex_deadline;
static uint32_t ex_req_frame;           /* the region the request arrived in */
static uint32_t ex_due_frame;           /* the region the staged turn belongs to */
static uint32_t ex_deferred;            /* grants moved to the next region */
static uint8_t  ex_retry;
static uint8_t  ex_waiting;             /* an event reply is outstanding */
static ipc_pair_rsp_evt_t ex_rsp;
static ipc_device_keys_t  ex_keys;

static uint32_t ex_reqs_forwarded, ex_rsp_sent, ex_confs_forwarded;
static uint32_t ex_accepts_sent, ex_paired, ex_cm7_refused, ex_timeouts;
static uint32_t ex_tx_err, ex_seal_err;
static uint32_t up_frames, up_ok, up_bad_slot, up_bad_frame, up_bad_tag;
static uint32_t up_replay;   /* authenticated, but not newer than the floor */
static uint32_t up_windows, up_sync;
/* Frames whose sync edge could not be attributed to them. ROADMAP item 44 */
static uint32_t up_sync_unpaired;
/* The downlink rotation, held across superframes.
 * radio_devices_docs/open_hub/radio/superloop.md */
static uint8_t  dl_next_slot;
static uint32_t dl_sent, dl_seal_err, dl_tx_err, dl_no_device, dl_served;
static uint32_t dl_prf_err, dl_last_hz, dl_last_sf;

/* pair_v4's invitation. Opaque here: CM4 keys the bytes it was given. ADR-0021 */
static uint8_t  pi_frame[RADIO_PAIR_INIT_MAX];
static uint8_t  pi_len;
static uint32_t pi_superframe;      /* 0 when nothing is queued */
static uint32_t pi_given, pi_sent, pi_missed, pi_tx_err, pi_replaced;
static uint32_t pi_frf;      /* RegFrf read back after the transmit */
static uint8_t  pi_paylen;
static uint32_t pi_last_sent_sf;
static uint32_t dl_opportunities;
static uint32_t dl_nonce_refused;
static int16_t  up_rssi_peak_x2 = -32768, up_rssi_floor_x2 = 32767;
static uint8_t  up_grid;            /* the channel the open window is tuned to */
/* Summed per received frame, for a fit of the correction against the channel.
 * radio_devices_docs/open_hub/radio/configuration.md */
static uint32_t afc_n, afc_read_err;
static int32_t  afc_last_hz, afc_min_hz, afc_max_hz;
static uint8_t  afc_last_grid;
static int64_t  afc_sum_hz, afc_sum_g, afc_sum_gg, afc_sum_gh;
static uint8_t  afc_ring_grid[IPC_AFC_RING];
static uint8_t  afc_ring_slot[IPC_AFC_RING];
static uint8_t  afc_ring_gain[IPC_AFC_RING];
static int8_t   afc_ring_rssi[IPC_AFC_RING];
static int32_t  afc_ring_afc_hz[IPC_AFC_RING];
static uint16_t afc_ring_crc_ok;    /* bit i: ring entry i passed its CRC */
static uint16_t afc_ring_in_frame;  /* bit i: entry i's level was taken during it */
static uint8_t  afc_ring_head;      /* where the next sample goes */
/* One frame's level, taken at its sync match and waiting for its PayloadReady.
 * radio_devices_docs/open_hub/radio/configuration.md */
static int8_t   sync_rssi_dbm;
static uint8_t  sync_slot;          /* which slot the edge landed in, 0xFF if none */
static uint8_t  sync_rssi_have;     /* 0 once consumed, so no frame borrows another's */
static uint16_t sync_rssi_lag_us;   /* from the DIO3 edge to the sample */
static uint16_t sync_rssi_lag_max_us;
static uint32_t sync_rssi_taken, sync_rssi_late, sync_rssi_err;
static uint32_t rx_crc_err;
static uint32_t rx_flushes;         /* receivers restarted after an undrainable FIFO */
static uint32_t rx_sync_match, rx_frames;

/* SyncAddressMatch as a hardware edge; rx_sync_match counts, this one times. */
static uint32_t sync_edges;
static uint32_t sync_edge_tk;
/* The boundary the stamp is measured against, reconstructed at the poll.
 * radio_devices_docs/radio/phy-seam.md */
static uint32_t sync_edge_base;
/* The last edge folded in, so an edge is serviced once and never twice. */
static uint32_t sync_seq_seen;
static uint32_t sync_last_offset_us;
/* The raw delta and the scale that converted it.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static uint32_t sync_last_offset_tk;
static int32_t  sync_last_ppm;
static uint32_t sync_min_offset_us = 0xFFFFFFFFu;
static uint32_t sync_max_offset_us;
/* Sums, so a spread is reported rather than a range: a range grows with n.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static uint32_t sync_ref_us;        /* deviations are from the first sample */
static uint8_t  sync_ref_set;
static uint32_t sync_stat_n;
static int64_t  sync_sum_d;
static uint64_t sync_sumsq_d;
static uint64_t sync_lead_sum, sync_lead_sumsq;
static int64_t  sync_cov_sum;       /* arrival against the beacon that preceded it */
static uint32_t sync_unpaired;      /* edges with no beacon of their own superframe */
static uint32_t sync_last_superframe;
static uint32_t sync_implausible;
static uint8_t  rx_last_len, rx_last_type;
static int8_t   rx_last_rssi;       /* off the RSSI latch, which nothing here triggers. ROADMAP item 14 */
/* What the refused frame actually carried, not which field mismatched. */
static uint16_t reqs_drop_net;
static uint32_t reqs_drop_hub, reqs_drop_dev;
static uint8_t  reqs_drop_head[16];
static uint8_t  reqs_drop_key[8];
static uint32_t rx_last_superframe;
/* A stronger signal is a larger (less negative) number.
 * radio_devices_docs/open_hub/radio/configuration.md */
static int16_t  rx_rssi_peak_x2 = -32768;   /* strongest sample seen */
static int16_t  rx_rssi_floor_x2 = 32767;   /* weakest */
/* Peak and floor read 0 for two different faults; this separates them. */
static uint32_t rx_rssi_samples;
static uint8_t  beacon_err_last, reqs_drop_last;

/* The uplink region is one long receive: the grid separates devices, not the hub.
 * radio_devices_docs/radio/tdma.md */
static uint8_t  uplink_rx_open;

extern CRYP_HandleTypeDef hcryp;
static hop_ctx_t hop;

/* The network hop key, a placeholder until CM7 installs the real one. */
static uint8_t  net_hop_key[RADIO_HOP_KEY_BYTES];

/* One AES-128 block through CRYP, inheriting nothing from the frame cipher.
 * radio_devices_docs/radio/hopping.md */
static int aes_ecb_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    CRYP_ConfigTypeDef cfg;
    uint32_t key_words[4];

    for (unsigned i = 0; i < 4u; i++)
        key_words[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
                       ((uint32_t)key[i * 4 + 2] << 8) | (uint32_t)key[i * 4 + 3];

    if (HAL_CRYP_GetConfig(&hcryp, &cfg) != HAL_OK)
        return -1;
    cfg.Algorithm     = CRYP_AES_ECB;
    cfg.KeySize       = CRYP_KEYSIZE_128B;
    cfg.pKey          = key_words;
    /* 8-bit, not the 32-bit the .ioc configures.
     * radio_devices_docs/radio/hopping.md */
    cfg.DataType      = CRYP_DATATYPE_8B;
    cfg.DataWidthUnit = CRYP_DATAWIDTHUNIT_BYTE;
    cfg.HeaderSize    = 0;
    if (HAL_CRYP_SetConfig(&hcryp, &cfg) != HAL_OK)
        return -1;
    /* 16 because the width unit above says bytes; the HAL reports neither error. */
    if (HAL_CRYP_Encrypt(&hcryp, (uint32_t *)(void *)in, 16,
                         (uint32_t *)(void *)out, 50) != HAL_OK)
        return -1;
    return 0;
}

/* Once per hop cycle. A PRF failure must propagate, never fall back.
 * radio_devices_docs/radio/hopping.md */
static int hop_prf_aes(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    (void)ctx;
    return aes_ecb_block(net_hop_key, in, out);
}

/* The hop key before any device has paired. Not secret, and not zeros.
 * radio_devices_docs/radio/hopping.md */
static void hop_key_placeholder(void) {
    for (unsigned i = 0; i < RADIO_HOP_KEY_BYTES; i++)
        net_hop_key[i] = (uint8_t)((hub_id >> (8u * (i & 3u))) ^ (0x5Au + i));
}

/* The vectors pin one channel count; the grid must still be it. ROADMAP item 81. */
_Static_assert(HOP_VEC_COUNT == RADIO_HOP_COUNT,
               "hop_v1 describes a deck this grid no longer draws");

/* The published key, so the deck stage runs the real PRF rather than a replay. */
static int hop_prf_kat(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    (void)ctx;
    return aes_ecb_block(HV_HOP_KEY, in, out);
}

/* Both layers against hop_v1, at boot, after the frame cipher had CRYP.
 * radio_devices_docs/radio/hopping.md */
static int hop_selftest(void) {
    uint8_t out[16];
    hop_ctx_t kat;
    uint8_t ch;

    /* FIPS-197 C.1 on CRYP: the hardware arm the host vector cannot supply.
     * radio_devices_docs/radio/hopping.md */
    if (aes_ecb_block(HV_FIPS_KEY, HV_FIPS_IN, out) != 0)
        return -1;
    if (memcmp(out, HV_FIPS_OUT, sizeof(out)) != 0)
        return -2;

    /* Cycle 1, not cycle 0: cycle 0 is identical under either endian convention.
     * radio_devices_docs/radio/hopping.md */
    if (aes_ecb_block(HV_HOP_KEY, HV_PRF_IN, out) != 0)
        return -3;
    if (memcmp(out, HV_PRF_OUT, sizeof(out)) != 0)
        return -4;

    /* The deck, which the PRF stage above cannot see. ROADMAP item 81. */
    if (hop_init(&kat, hop_prf_kat, NULL, HOP_VEC_COUNT) != 0)
        return -5;
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&kat, i, &ch) != 0)
            return -6;
        if (ch != HV_DECK0[i])
            return -7;
    }
    for (uint32_t i = 0; i < HOP_VEC_COUNT; i++) {
        if (hop_channel(&kat, HOP_VEC_COUNT + i, &ch) != 0)
            return -8;
        if (ch != HV_DECK1[i])
            return -9;
    }

    /* Cycles the deck stage never reaches, so the counter split is checked too. */
    for (unsigned i = 0; i < sizeof(HV_SAMPLE_CH); i++) {
        if (hop_channel(&kat, HV_SAMPLE_SF[i], &ch) != 0)
            return -10;
        if (ch != HV_SAMPLE_CH[i])
            return -11;
    }
    return 0;
}

static uint32_t slot_hz(uint32_t slot) {
    return RADIO_SLOT_HZ(slot);
}

/* Skips the reserved join slot, so the two sets are disjoint by construction. */
static uint32_t hop_slot_to_grid(uint8_t hop_index) {
    return RADIO_HOP_TO_GRID(hop_index);
}

uint8_t RFM_Init(uint8_t network_id, uint8_t node_id) {
    (void)network_id;

    /* Twenty configuration calls, each run once, now live behind the seam.
     * radio_devices_docs/radio/phy-seam.md */
    if (phy_init() != 0)
        return 1;
    /* The hub's own address, not a PHY constant; inert while filtering is off. */
    if (rfm69_set_node_address(phy_rfm69_dev(), node_id) != RFM69_OK)
        return 1;
    /* phy.h: the caller names the channel, never the layer below it. */
    if (phy_tune(slot_hz(RADIO_JOIN_SLOT)) != 0)
        return 1;

    if (hop_init(&hop, hop_prf_aes, NULL, RADIO_HOP_COUNT) != 0)
        return 1;

    /* Before the grid starts, against published frames rather than a round trip.
     * radio_devices_docs/radio/crypto/wire-crypto.md */
    aead_selftest_rc = aead_selftest();
    /* After the frame cipher, so the PRF runs from the adversarial CRYP state. */
    if (aead_selftest_rc == 0) {
        /* 51..61 carry the stage, so a failure names its layer. Item 81. */
        int hop_rc = hop_selftest();
        if (hop_rc != 0)
            aead_selftest_rc = -50 + hop_rc;
    }
    if (aead_selftest_rc == 0 && frame_selftest() != 0)
        aead_selftest_rc = -30;

    /* Not the network's key, and not zeros - see hop_key_placeholder(). */
    hop_key_placeholder();

    /* Never restart the protocol's clock at zero.
     * radio_devices_docs/open_hub/arch/keystore.md */
    frame_counter = kv_reserved();
    /* Reserve before the grid starts, so the first beacon is already covered. */
    (void)kv_reserve(frame_counter);

    return 0;
}

/* The lead statistics are the hub's, not the PHY's.
 * radio_devices_docs/radio/phy-seam.md */
static int frame_send(const void *payload, uint8_t len) {
    uint32_t span_tk = 0;
    int rc = phy_transmit(payload, len, &span_tk);

    if (rc == 0) {
        /* An upper bound: it carries ramp-down and the PacketSent poll too. */
        uint32_t span = timebase_ticks_to_us(span_tk);
        uint32_t air  = RADIO_AIR_START_TO_END_US(len);

        lead_last_us = (span > air) ? (span - air) : 0u;
        if (lead_last_us < lead_min_us) lead_min_us = lead_last_us;
        if (lead_last_us > lead_max_us) lead_max_us = lead_last_us;
        lead_n++;
    }
    return rc;
}

/* An absolute grid: a fixed step from the last boundary, and the counter advances
 * here alone. radio_devices_docs/open_hub/radio/superloop.md */
static int superframe_due(void) {
    if (!grid_started) {
        superframe_start_tk = rfm_micros();
        superframe_tk = timebase_us_to_ticks(SUPERFRAME_US);
        join_offset_tk = timebase_us_to_ticks(RADIO_JOIN_OFFSET_US);
        grid_started = 1;
        return 0;
    }
    if (!timebase_elapsed(superframe_start_tk + superframe_tk))
        return 0;

    superframe_start_tk += superframe_tk;
    frame_counter++;

    /* Re-read after the boundary, so it moves the next interval, not this one. */
    superframe_tk = timebase_us_to_ticks(SUPERFRAME_US);
    join_offset_tk = timebase_us_to_ticks(RADIO_JOIN_OFFSET_US);

    /* Step the grid forward rather than catch up: it is a schedule, not a queue. */
    while (timebase_elapsed(superframe_start_tk + superframe_tk)) {
        superframe_start_tk += superframe_tk;
        frame_counter++;
    }
    return 1;
}


static void RFM_send_broadcast(uint8_t flags, uint8_t resume_in) {
    radio_data_beacon_t payload;
    uint8_t hop_idx;
    int rc;

    /* How far past the boundary this beacon leaves; devices inherit it directly.
     * radio_devices_docs/open_hub/radio/timebase.md */
    late_last_us = rfm_micros() - superframe_start_tk;
    if (late_last_us > late_max_us) late_max_us = late_last_us;
    if (late_last_us < late_min_us) late_min_us = late_last_us;
    if (late_last_us > timebase_us_to_ticks(RADIO_BEACON_LATE_LIMIT_US))
        late_over++;

    memset(&payload, 0, sizeof(payload));
    payload.type       = RADIO_FRAME_DATA_BEACON;
    payload.version    = RADIO_PROTO_VERSION;
    payload.net_id     = RADIO_NET_ID;
    payload.hub_id     = hub_id;
    payload.superframe = frame_counter;
    payload.flags      = flags;
    payload.resume_in  = resume_in;
    data_beacons++;
    if (flags & RADIO_BEACON_FLAG_QUIESCE)
        announce_beacons++;

    /* One hop per superframe, from a keyed shuffle; a PRF failure means silence.
     * radio_devices_docs/radio/hopping.md */
    if (hop_channel(&hop, frame_counter, &hop_idx) != 0) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_PRF;
        return;
    }
    if (phy_tune(slot_hz(hop_slot_to_grid(hop_idx))) != 0) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_RETUNE;
        return;
    }
    rc = frame_send(&payload, (uint8_t)sizeof(payload));
    if (rc != 0) {
        beacon_err++;
        /* -2 is a payload the part cannot hold, which is not a failed transmit. */
        beacon_err_last = (rc == -2) ? RADIO_BERR_BUILD : RADIO_BERR_TX;
        return;
    }

    /* Held for this superframe's uplinks to pair against. */
    bl_last_us = lead_last_us;
    bl_sf      = frame_counter;
    bl_n++;
    if (bl_last_us < bl_min_us) bl_min_us = bl_last_us;
    if (bl_last_us > bl_max_us) bl_max_us = bl_last_us;
}

/* Fixed channel, cleartext, and carrying nothing secret.
 * radio_devices_docs/radio/joining.md */
static void RFM_send_join_beacon(void) {
    radio_join_beacon_t payload;

    /* Whole struct first: a field added later must not go out off the stack. */
    memset(&payload, 0, sizeof(payload));
    payload.type         = RADIO_FRAME_JOIN_BEACON;
    payload.version      = RADIO_PROTO_VERSION;
    payload.net_id       = RADIO_NET_ID;
    payload.hub_id       = hub_id;
    payload.superframe   = frame_counter;
    payload.flags        = RADIO_JOIN_FLAG_WINDOW_OPEN;
    payload.hop_channels = RADIO_HOP_COUNT;

    if (phy_tune(slot_hz(RADIO_JOIN_SLOT)) != 0) {
        join_tx_err++;
        return;
    }
    join_beacons++;
    if (frame_send(&payload, (uint8_t)sizeof(payload)) != 0)
        join_tx_err++;
}

/* Only the superframe varies in a downlink's nonce, so it must be strictly newer.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
static uint8_t dl_nonce_is_new(const dev_entry_t *d, uint32_t sf) {
    if (!d->dl_nonce_used)
        return 1u;
    return ((int32_t)(sf - d->dl_nonce_sf) > 0) ? 1u : 0u;
}

/* One filler for the poll reply and the arrival event: two would drift. */
static void fill_report(ipc_device_report_t *d, const dev_entry_t *e) {
    memset(d, 0, sizeof(*d));
    d->dev_id          = e->dev_id;
    d->last_superframe = e->last_superframe;
    d->frames_ok       = e->frames_ok;
    d->frames_bad      = e->frames_bad;
    d->uptime_s        = e->uptime_s;
    d->total           = device_count;
    d->slot            = e->slot;
    d->rssi_up         = e->rssi_up;
    d->rssi_down       = e->rssi_down;
    /* Asked, granted and done are three facts; none stands in for another. */
    d->cmd_every       = (e->dl_cmd == RADIO_CMD_SET_RATE) ? e->dl_report_every : 0u;
    d->cmd_state       = (e->dl_cmd != RADIO_CMD_SET_RATE) ? 0u
                       : (e->dl_acked ? 2u : 1u);
    d->cyc_min         = e->cyc_min;
    d->cyc_n           = e->cyc_n;
    d->cyc_sum         = e->cyc_sum;
    d->supply_mv       = e->supply_mv;
    d->temp_c_x10      = e->temp_c_x10;
    d->missed_run      = e->missed_run;
    d->report_every    = e->report_every;
    d->flags           = e->flags;
    d->ack_arg         = e->dl_ack_arg;
    d->arrival_us      = e->arrival_us;
    d->arrival_sync_us = e->arrival_sync_us;
    d->sync_unpaired   = e->sync_unpaired;
}

/* A dropped notification and a silent device look alike, so count the drop. */
static void uplink_notify(const dev_entry_t *e) {
    ipc_device_report_t d;
    uint16_t seq = 0;

    fill_report(&d, e);
    if (ipc_send_event(IPC_EVT_UPLINK, &d, (uint8_t)sizeof(d), &seq) != 0) {
        up_evt_drop++;
        return;
    }
    up_evt_sent++;
    uint32_t since_sync = timebase_ticks_to_us(rfm_micros() - sync_edge_tk);

    /* An unanswered predecessor is lost, not silently replaced. */
    if (evt_waiting)
        evt_lost++;
    /* It fires with 42 bytes still on air, which the budget already charges.
     * ROADMAP item 2 */
    if (since_sync < RADIO_AIR_SYNC_TO_END_US(RADIO_UPLINK_BYTES)) {
        /* Sooner than the air allows: the edge belonged to another frame. */
        evt_arrival_bad++;
    } else {
        evt_arrival_last_us = since_sync -
                              RADIO_AIR_SYNC_TO_END_US(RADIO_UPLINK_BYTES);
        if (evt_arrival_last_us > evt_arrival_max_us)
            evt_arrival_max_us = evt_arrival_last_us;
    }
    evt_sent_tk = rfm_micros();
    evt_seq     = seq;
    evt_waiting = 1;
}

/* One reader only: the filtering poll discards what it does not match.
 * radio_devices_docs/open_hub/arch/ipc.md */
static void evt_reply_service(void) {
    ipc_msg_t m;
    uint32_t us;

    while (ipc_poll_any_event_reply(&m)) {
        /* The reply bounds CM7's half: it cannot answer before it has handled.
         * ROADMAP item 2 */
        if (evt_waiting && m.seq == evt_seq) {
            evt_waiting = 0;
            us = timebase_ticks_to_us(rfm_micros() - evt_sent_tk);
            evt_rtt_last_us = us;
            if (evt_replied == 0u || us < evt_rtt_min_us) evt_rtt_min_us = us;
            if (us > evt_rtt_max_us) evt_rtt_max_us = us;
            evt_rtt_sum_us += us;
            evt_replied++;
            continue;
        }
        /* Held rather than handled here: the exchange is a state machine. */
        if (ex_waiting && m.seq == ex_seq) {
            ex_reply     = m;
            ex_reply_new = 1;
            continue;
        }
        evt_stale++;
    }
}

/* One request, one reply, always - unhandled types included. */
static void RFM_serve_request(const ipc_msg_t *req) {
    uint8_t status = IPC_ST_OK;
    uint8_t reply = 0;
    uint8_t len = 0;

    switch (req->type) {
    case IPC_REQ_READ_REG:
        if (rfm69_read_reg(phy_rfm69_dev(), req->arg, &reply) != RFM69_OK)
            status = IPC_ST_RADIO_ERR;
        len = 1;
        break;
    case IPC_REQ_ADD_DEVICE: {
        uint32_t dev_id;

        if (req->len < sizeof(dev_id)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&dev_id, req->payload, sizeof(dev_id));
        reply = RFM_open_pairing(dev_id);
        len = 1;
        break;
    }
    case IPC_REQ_GET_TIMING: {
        ipc_timing_t t;

        t.superframe   = frame_counter;
        t.now_tk       = rfm_micros();
        t.late_last_us = timebase_ticks_to_us(late_last_us);
        t.late_max_us  = timebase_ticks_to_us(late_max_us);
        t.late_min_us  = (late_min_us == 0xFFFFFFFFu) ? 0
                                                      : timebase_ticks_to_us(late_min_us);
        t.period_us     = SUPERFRAME_US;
        t.period_tk     = superframe_tk;
        t.calib_ppm     = calib_ppm();
        t.calib_ppm_min = calib_ppm_min();
        t.calib_ppm_max = calib_ppm_max();
        t.span_lo       = calib_span_lo();
        t.span_hi       = calib_span_hi();
        t.calib_windows = calib_windows();
        t.calib_rejects = calib_rejects();
        t.calib_age_tk  = calib_age_tk();
        t.boot_wait_ms  = bootwait_ms();
        t.boot_wait_spins = bootwait_spins();
        memcpy(t.build, BUILD_ID BUILD_SUFFIX, sizeof(BUILD_ID BUILD_SUFFIX));
        t.late_over     = late_over;
        (void)ipc_send_reply(req, IPC_ST_OK, &t, (uint8_t)sizeof(t));
        return;
    }
    /* Reads the live guard at three superframes and seals nothing. */
    case IPC_REQ_DL_NONCE_PROBE: {
        ipc_dl_nonce_probe_t pr;
        const dev_entry_t *d = NULL;
        uint8_t i;

        memset(&pr, 0, sizeof(pr));
        for (i = 0; i < RADIO_MAX_DEVICES; i++) {
            if (devices[i].used && devices[i].dl_nonce_used) {
                d = &devices[i];
                break;
            }
        }
        if (d != NULL) {
            pr.dev_id       = d->dev_id;
            pr.last_sf      = d->dl_nonce_sf;
            pr.used         = 1u;
            pr.verdict_same = dl_nonce_is_new(d, d->dl_nonce_sf);
            pr.verdict_next = dl_nonce_is_new(d, d->dl_nonce_sf + 1u);
            pr.verdict_prev = dl_nonce_is_new(d, d->dl_nonce_sf - 1u);
        }
        (void)ipc_send_reply(req, IPC_ST_OK, &pr, (uint8_t)sizeof(pr));
        return;
    }
    /* The real PRF on a caller-supplied block, comparable against a host AES. */
    case IPC_REQ_HOP_PRF: {
        uint8_t in[16], out[16];

        if (req->len < sizeof(in)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(in, req->payload, sizeof(in));
        memset(out, 0, sizeof(out));
        hop_prf_aes(NULL, in, out);
        (void)ipc_send_reply(req, IPC_ST_OK, out, (uint8_t)sizeof(out));
        return;
    }
    case IPC_REQ_GET_PAIR_STATE: {
        ipc_pair_state_t p;
        uint32_t left_tk;

        p.state        = (uint8_t)pair_state;
        p.quiesce_left = (pair_state == RADIO_PAIR_QUIESCE &&
                          (int32_t)(quiesce_resume_at - frame_counter) > 0)
                         ? (uint8_t)(quiesce_resume_at - frame_counter) : 0u;
        p.dev_id       = pairing_dev_id;
        left_tk        = pairing_deadline_us - rfm_micros();
        p.window_left_ms = (pairing_open && (int32_t)left_tk > 0) ? left_tk / 1000u : 0u;
        p.resume_at    = quiesce_resume_at;
        p.reqs_seen    = pair_reqs_seen;
        p.reqs_dropped = pair_reqs_dropped;
        p.join_regions = join_regions;
        p.join_beacons = join_beacons;
        p.join_tx_err  = join_tx_err;
        p.data_beacons = data_beacons;
        p.announce_beacons = announce_beacons;
        p.silent_frames = silent_frames;
        p.quiesce_refused = quiesce_refused;
        p.beacon_err      = beacon_err;
        p.beacon_err_last = beacon_err_last;
        p.reqs_drop_last  = reqs_drop_last;
        p.quiesce_lost    = quiesce_lost;
        (void)ipc_send_reply(req, IPC_ST_OK, &p, (uint8_t)sizeof(p));
        return;
    }
    /* Test scaffolding: a quiesce is otherwise reachable only by a real device.
     * radio_devices_docs/open_hub/testing/sdr.md */
    case IPC_REQ_QUIESCE:
        reply = begin_quiesce(req->arg);
        len = 1;
        break;
    case IPC_REQ_GET_STORE: {
        ipc_store_state_t k;

        k.reserved   = kv_reserved();
        k.counter    = frame_counter;
        k.writes     = kv_writes();
        k.errors     = kv_errors();
        k.slots_left = kv_slots_left();
        k.unreserved = unreserved_frames;
        (void)ipc_send_reply(req, IPC_ST_OK, &k, (uint8_t)sizeof(k));
        return;
    }
    case IPC_REQ_KV_TORN:
        reply = (kv_write_torn() == 0) ? 1u : 0u;
        len = 1;
        break;
    /* CM7 replaying its store into a freshly booted radio core.
     * radio_devices_docs/open_hub/arch/keystore.md */
    case IPC_REQ_INSTALL_DEVICE: {
        ipc_device_keys_t k;

        if (req->len < sizeof(k)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&k, req->payload, sizeof(k));
        if (install_device(&k) != 0)
            status = IPC_ST_BAD_ARG;
        len = 0;
        break;
    }
    case IPC_REQ_SET_REPORT_RATE:
        report_every_grant = (req->arg == 0u) ? 1u : req->arg;
        reply = report_every_grant;
        len = 1;
        break;
    case IPC_REQ_GET_SYNCTIME: {
        ipc_synctime_t s;

        memset(&s, 0, sizeof(s));
        s.edges           = sync_edges;
        s.frames          = rx_frames;   /* same read as edges: the ladder's denominator */
        s.implausible     = sync_implausible;
        s.last_offset_us  = sync_last_offset_us;
        s.min_offset_us   = (sync_min_offset_us == 0xFFFFFFFFu) ? 0u
                                                                : sync_min_offset_us;
        s.max_offset_us   = sync_max_offset_us;
        s.last_superframe = sync_last_superframe;
        s.dio_map1        = phy_rfm69_dio_map1();
        s.dio3_asked      = RFM69_DIO3_SYNC_ADDRESS;
        s.lead_last_us    = lead_last_us;
        s.lead_min_us     = (lead_min_us == 0xFFFFFFFFu) ? 0u : lead_min_us;
        s.lead_max_us     = lead_max_us;
        s.lead_n          = lead_n;
        s.last_offset_tk  = sync_last_offset_tk;
        s.calib_ppm       = sync_last_ppm;
        (void)ipc_send_reply(req, IPC_ST_OK, &s, (uint8_t)sizeof(s));
        return;
    }

    case IPC_REQ_GET_AFC: {
        ipc_afc_t a;

        memset(&a, 0, sizeof(a));
        a.n         = afc_n;
        a.read_err  = afc_read_err;
        a.last_hz   = afc_last_hz;
        a.min_hz    = afc_min_hz;
        a.max_hz    = afc_max_hz;
        a.last_grid = afc_last_grid;
        a.sum_hz    = afc_sum_hz;
        a.sum_g     = afc_sum_g;
        a.sum_gg    = afc_sum_gg;
        a.sum_gh    = afc_sum_gh;
        (void)ipc_send_reply(req, IPC_ST_OK, &a, (uint8_t)sizeof(a));
        return;
    }

    /* The filter's own width, which the encoder rounds up and nothing has chosen.
     * radio_devices_docs/open_hub/radio/configuration.md */
    case IPC_REQ_SET_RXBW: {
        ipc_rxbw_t b;
        uint32_t hz = 0;
        uint8_t back = 0;

        if (req->len < sizeof(hz)) {
            status = IPC_ST_BAD_ARG;
            len = 0;
            break;
        }
        memcpy(&hz, req->payload, sizeof(hz));
        memset(&b, 0, sizeof(b));
        b.asked_hz = hz;
        if (rfm69_set_rx_bandwidth_hz(phy_rfm69_dev(), hz) != RFM69_OK ||
            rfm69_read_reg(phy_rfm69_dev(), RFM69_RegRxBw, &back) != RFM69_OK) {
            status = IPC_ST_RADIO_ERR;
            len = 0;
            break;
        }
        b.reg    = back;
        b.set_hz = rfm69_rx_bandwidth_from_reg(back);
        (void)ipc_send_reply(req, IPC_ST_OK, &b, (uint8_t)sizeof(b));
        return;
    }

    /* The one knob that separates an overdriven front end from a dirty transmitter. */
    case IPC_REQ_SET_LNA: {
        if (rfm69_set_lna_gain(phy_rfm69_dev(), req->arg) != RFM69_OK)
            status = IPC_ST_BAD_ARG;
        len = 0;
        break;
    }

    case IPC_REQ_GET_EVT_LAT: {
        ipc_evt_latency_t l;

        memset(&l, 0, sizeof(l));
        l.sent            = up_evt_sent;
        l.replied         = evt_replied;
        l.lost            = evt_lost;
        l.arrival_last_us = evt_arrival_last_us;
        l.arrival_max_us  = evt_arrival_max_us;
        l.rtt_last_us     = evt_rtt_last_us;
        l.rtt_min_us      = (evt_replied == 0u) ? 0u : evt_rtt_min_us;
        l.rtt_max_us      = evt_rtt_max_us;
        l.rtt_sum_us      = evt_rtt_sum_us;
        l.stale           = evt_stale;
        l.arrival_bad     = evt_arrival_bad;
        (void)ipc_send_reply(req, IPC_ST_OK, &l, (uint8_t)sizeof(l));
        return;
    }

    case IPC_REQ_GET_JOINPROBE: {
        ipc_join_probe_t j;

        memset(&j, 0, sizeof(j));
        j.windows    = jp_windows;
        j.passes     = jp_passes;
        j.probes     = jp_probes;
        j.not_rx     = jp_not_rx;
        j.inv_probes = jp_inv_probes;
        j.inv_not_rx = jp_inv_not_rx;
        j.levels     = jp_levels;
        j.tries      = jp_level_tries;
        j.inv_peak   = (jp_inv_peak_x2 == -32768) ? 0 : (int8_t)(jp_inv_peak_x2 / 2);
        j.inv_floor  = (jp_inv_floor_x2 == 32767) ? 0 : (int8_t)(jp_inv_floor_x2 / 2);
        j.idle_peak  = (jp_idle_peak_x2 == -32768) ? 0 : (int8_t)(jp_idle_peak_x2 / 2);
        j.idle_floor = (jp_idle_floor_x2 == 32767) ? 0 : (int8_t)(jp_idle_floor_x2 / 2);
        j.last_op    = jp_last_op;
        (void)ipc_send_reply(req, IPC_ST_OK, &j, (uint8_t)sizeof(j));
        return;
    }

    case IPC_REQ_GET_AFC_RAW: {
        ipc_afc_raw_t r;
        uint32_t i, have = (afc_n < IPC_AFC_RING) ? afc_n : IPC_AFC_RING;

        memset(&r, 0, sizeof(r));
        r.total  = afc_n;
        r.n      = (uint8_t)have;
        /* Newest first, walking back from the head. */
        for (i = 0; i < have; i++) {
            uint32_t k = (afc_ring_head + IPC_AFC_RING - 1u - i) % IPC_AFC_RING;

            r.grid[i] = afc_ring_grid[k];
            r.slot[i] = afc_ring_slot[k];
            r.gain[i] = afc_ring_gain[k];
            r.rssi[i] = afc_ring_rssi[k];
            r.afc_hz[i] = afc_ring_afc_hz[k];
            if ((afc_ring_crc_ok & (1u << k)) != 0u)
                r.crc_ok = (uint16_t)(r.crc_ok | (1u << i));
            if ((afc_ring_in_frame & (1u << k)) != 0u)
                r.in_frame = (uint16_t)(r.in_frame | (1u << i));
        }
        r.lag_max_us = sync_rssi_lag_max_us;
        /* Saturated rather than truncated: a wrapped count reads as a small one. */
        r.rssi_taken = (sync_rssi_taken > 0xFFFFu) ? 0xFFFFu : (uint16_t)sync_rssi_taken;
        r.rssi_late  = (sync_rssi_late  > 0xFFFFu) ? 0xFFFFu : (uint16_t)sync_rssi_late;
        r.rssi_err   = (sync_rssi_err   > 0xFFFFu) ? 0xFFFFu : (uint16_t)sync_rssi_err;
        (void)ipc_send_reply(req, IPC_ST_OK, &r, (uint8_t)sizeof(r));
        return;
    }

    case IPC_REQ_GET_SYNCSTATS: {
        ipc_syncstats_t s;

        memset(&s, 0, sizeof(s));
        s.ref_us        = sync_ref_us;
        s.n             = sync_stat_n;
        s.sum_d         = sync_sum_d;
        s.sumsq_d       = sync_sumsq_d;
        s.lead_sum      = sync_lead_sum;
        s.lead_sumsq    = sync_lead_sumsq;
        s.cov_sum       = sync_cov_sum;
        s.unpaired      = sync_unpaired;
        s.beacon_n      = bl_n;
        s.beacon_min_us = (bl_min_us == 0xFFFFFFFFu) ? 0u : bl_min_us;
        s.beacon_max_us = bl_max_us;
        (void)ipc_send_reply(req, IPC_ST_OK, &s, (uint8_t)sizeof(s));
        return;
    }

    case IPC_REQ_GET_RXDIAG: {
        ipc_rx_diag_t d;

        memset(&d, 0, sizeof(d));
        d.sync_match      = rx_sync_match;
        d.crc_err         = rx_crc_err;
        d.frames          = rx_frames;
        d.last_superframe = rx_last_superframe;
        d.last_len        = rx_last_len;
        d.last_type       = rx_last_type;
        d.flushes         = rx_flushes;
        d.last_rssi       = rx_last_rssi;
        d.rssi_peak       = (rx_rssi_peak_x2 == -32768) ? 0 : (int8_t)(rx_rssi_peak_x2 / 2);
        d.rssi_floor      = (rx_rssi_floor_x2 == 32767) ? 0 : (int8_t)(rx_rssi_floor_x2 / 2);
        d.up_rssi_peak    = (up_rssi_peak_x2 == -32768) ? 0 : (int8_t)(up_rssi_peak_x2 / 2);
        d.up_rssi_floor   = (up_rssi_floor_x2 == 32767) ? 0 : (int8_t)(up_rssi_floor_x2 / 2);
        d.rssi_samples    = rx_rssi_samples;
        d.drop_net_id     = reqs_drop_net;
        d.drop_hub_id     = reqs_drop_hub;
        d.drop_dev_id     = reqs_drop_dev;
        memcpy(d.drop_head, reqs_drop_head, sizeof(d.drop_head));
        memcpy(d.drop_key,  reqs_drop_key,  sizeof(d.drop_key));
        (void)ipc_send_reply(req, IPC_ST_OK, &d, (uint8_t)sizeof(d));
        return;
    }
    /* The only writable block as wide as a frame, so it exercises the real read. */
    case IPC_REQ_SPI_LOOP: {
        ipc_spi_loop_t r;
        uint8_t pattern[57], back[57];
        uint32_t n = 0, i, pass;

        memset(&r, 0, sizeof(r));
        if (req->len >= 4u) memcpy(&n, req->payload, 4);
        if (n == 0u || n > 2000u) n = 200u;

        /* Alternating high-bit bytes, so bit 7 shows in both directions. */
        for (i = 0; i < sizeof(pattern); i++)
            pattern[i] = (i & 1u) ? 0xA5u : 0x5Au;

        (void)phy_standby();

        /* RegAesKey holds 16 bytes verbatim: the control arm for the FIFO test. */
        for (pass = 0; pass < n; pass++) {
            uint8_t rb[16];

            if (rfm69_write(phy_rfm69_dev(), RFM69_RegAesKey1, pattern, 16) != RFM69_OK ||
                rfm69_read(phy_rfm69_dev(), RFM69_RegAesKey1, rb, 16) != RFM69_OK) {
                r.io_err++;
                continue;
            }
            for (i = 0; i < 16u; i++) {
                if (rb[i] == pattern[i])
                    continue;
                if (r.reg_bad_bytes == 0u)
                    memcpy(r.reg_read, rb + (i & ~7u), 8);
                r.reg_bad_bytes++;
                if ((rb[i] ^ pattern[i]) == 0x80u)
                    r.reg_xor_80++;
            }
        }

        for (pass = 0; pass < n; pass++) {
            uint8_t bad = 0;

            memset(back, 0, sizeof(back));
            if (rfm69_write_fifo(phy_rfm69_dev(), pattern, sizeof(pattern)) != RFM69_OK ||
                rfm69_read_fifo(phy_rfm69_dev(), back, sizeof(back)) != RFM69_OK) {
                r.io_err++;
                continue;
            }
            for (i = 0; i < sizeof(pattern); i++) {
                if (back[i] == pattern[i])
                    continue;
                r.bad_bytes++;
                if ((back[i] ^ pattern[i]) == 0x80u)
                    r.xor_80++;
                if (!bad && r.bad_passes == 0u) {
                    memcpy(r.first_bad, back + (i & ~7u), 8);
                    memcpy(r.expect, pattern + (i & ~7u), 8);
                }
                bad = 1;
            }
            if (bad) r.bad_passes++;
            r.passes++;
        }
        {
            /* Both halves measured: kernel clock from RCC, divider from CFG1. */
            uint32_t mbr = (SPI1->CFG1 & SPI_CFG1_MBR) >> SPI_CFG1_MBR_Pos;

            r.spi_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI123)
                       >> (mbr + 1u);
        }
        (void)ipc_send_reply(req, IPC_ST_OK, &r, (uint8_t)sizeof(r));
        return;
    }
    case IPC_REQ_HOP_AT: {
        ipc_hop_at_t h;
        uint8_t idx = 0;
        uint32_t sf = frame_counter;

        if (req->len >= 4u) memcpy(&sf, req->payload, 4);
        memset(&h, 0, sizeof(h));
        h.superframe  = sf;
        h.placeholder = net_hop_key_set ? 0u : 1u;
        memcpy(h.key_head, net_hop_key, sizeof(h.key_head));
        if (hop_channel(&hop, sf, &idx) != 0) {
            status = IPC_ST_RADIO_ERR;
            break;
        }
        h.channel   = idx;
        h.grid_slot = (uint8_t)hop_slot_to_grid(idx);
        h.hz        = slot_hz(hop_slot_to_grid(idx));
        h.count     = hop.count;
        memcpy(h.deck, hop.deck, sizeof(h.deck) < sizeof(hop.deck)
                                 ? sizeof(h.deck) : sizeof(hop.deck));
        (void)ipc_send_reply(req, IPC_ST_OK, &h, (uint8_t)sizeof(h));
        return;
    }
    case IPC_REQ_SET_PAIR_INIT: {
        ipc_pair_init_t p;

        if (req->len < sizeof(p)) { status = IPC_ST_BAD_ARG; break; }
        memcpy(&p, req->payload, sizeof(p));
        if (p.len == 0u || p.len > sizeof(pi_frame) || p.superframe == 0u) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        /* A displaced frame is counted: CM7 is building faster than the grid sends. */
        if (pi_superframe != 0u)
            pi_replaced++;
        memcpy(pi_frame, p.frame, p.len);
        pi_len = p.len;
        pi_superframe = p.superframe;
        pi_given++;
        break;
    }

    case IPC_REQ_GET_PAIR_INIT: {
        ipc_pair_init_state_t p;

        memset(&p, 0, sizeof(p));
        p.given        = pi_given;
        p.sent         = pi_sent;
        p.missed       = pi_missed;
        p.tx_err       = pi_tx_err;
        p.replaced     = pi_replaced;
        p.last_sent_sf = pi_last_sent_sf;
        p.pending_sf   = pi_superframe;
        p.frf          = pi_frf;
        p.payload_len  = pi_paylen;
        (void)ipc_send_reply(req, IPC_ST_OK, &p, (uint8_t)sizeof(p));
        return;
    }

    case IPC_REQ_GET_DOWNLINK: {
        ipc_downlink_state_t d;

        memset(&d, 0, sizeof(d));
        d.opportunities = dl_opportunities;
        d.sent          = dl_sent;
        d.seal_err      = dl_seal_err;
        d.tx_err        = dl_tx_err;
        d.no_device     = dl_no_device;
        d.nonce_refused = dl_nonce_refused;
        d.prf_err       = dl_prf_err;
        d.last_hz       = dl_last_hz;
        d.last_superframe = dl_last_sf;
        d.next_slot     = dl_next_slot;
        d.cmd_sent      = dl_cmd_sent;
        d.cmd_replaced  = dl_cmd_replaced;
        d.cmd_acked     = dl_cmd_acked;
        d.cmd_lost      = dl_cmd_lost;
        (void)ipc_send_reply(req, IPC_ST_OK, &d, (uint8_t)sizeof(d));
        return;
    }
    case IPC_REQ_GET_VECTORS: {
        ipc_vectors_t v;

        memset(&v, 0, sizeof(v));
        /* Compiled in, so it reports what this core was built with. */
        memcpy(v.pair, PAIR_VECTORS_DIGEST, sizeof(PAIR_VECTORS_DIGEST));
        memcpy(v.hop, HOP_VECTORS_DIGEST, sizeof(HOP_VECTORS_DIGEST));
        v.pair_version = PAIR_VECTORS_VERSION;
        (void)ipc_send_reply(req, IPC_ST_OK, &v, (uint8_t)sizeof(v));
        return;
    }
    case IPC_REQ_GET_EXCHANGE: {
        ipc_exchange_state_t x;

        memset(&x, 0, sizeof(x));
        x.state           = (uint8_t)ex_state;
        x.aead_selftest   = (uint8_t)(aead_selftest_rc == 0 ? 0 : -aead_selftest_rc);
        x.devices         = device_count;
        x.report_every    = report_every_grant;
        x.dev_id          = ex_dev_id;
        x.reqs_forwarded  = ex_reqs_forwarded;
        x.rsp_sent        = ex_rsp_sent;
        x.confs_forwarded = ex_confs_forwarded;
        x.accepts_sent    = ex_accepts_sent;
        x.paired          = ex_paired;
        x.cm7_refused     = (uint16_t)ex_cm7_refused;
        x.timeouts        = (uint16_t)ex_timeouts;
        x.tx_err          = (uint16_t)ex_tx_err;
        x.seal_err        = (uint16_t)ex_seal_err;
        x.uplink_windows  = up_windows;
        x.uplink_sync     = up_sync;
        x.uplink_evt_drop = up_evt_drop;
        x.uplink_sync_unpaired = up_sync_unpaired;
        x.uplink_frames   = up_frames;
        x.uplink_ok       = up_ok;
        x.uplink_bad_slot = up_bad_slot;
        x.uplink_bad_frame = up_bad_frame;
        x.uplink_bad_tag  = up_bad_tag;
        x.uplink_replay   = up_replay;
        (void)ipc_send_reply(req, IPC_ST_OK, &x, (uint8_t)sizeof(x));
        return;
    }
    /* The index-th live device; total rides along to size the list in one call. */
    case IPC_REQ_GET_DEVICE_INFO: {
        ipc_device_report_t d;
        uint32_t seen = 0;
        uint32_t want;
        uint32_t i;

        want = req->arg;
        for (i = 0; i < RADIO_MAX_DEVICES; i++) {
            if (!devices[i].used)
                continue;
            if (seen++ != want)
                continue;

            fill_report(&d, &devices[i]);
            (void)ipc_send_reply(req, IPC_ST_OK, &d, (uint8_t)sizeof(d));
            return;
        }
        status = IPC_ST_BAD_ARG;
        break;
    }
    case IPC_REQ_SET_DEVICE_PARAM: {
        ipc_device_cmd_t c;
        uint32_t i;

        if (req->len < sizeof(c)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&c, req->payload, sizeof(c));
        status = IPC_ST_BAD_ARG;
        for (i = 0; i < RADIO_MAX_DEVICES; i++) {
            if (!devices[i].used || devices[i].dev_id != c.dev_id)
                continue;
            /* Replacing one still in flight is counted: the old one never went. */
            if (devices[i].dl_repeats > 0u)
                dl_cmd_replaced++;
            devices[i].dl_cmd          = c.cmd;
            devices[i].dl_report_every = c.report_every;
            devices[i].dl_arg          = c.arg;
            devices[i].dl_repeats      = c.repeats;
            /* Wraps past RADIO_CMD_SEQ_NONE: a zero on the wire is silence. */
            devices[i].dl_cmd_seq++;
            if (devices[i].dl_cmd_seq == RADIO_CMD_SEQ_NONE)
                devices[i].dl_cmd_seq = 1u;
            devices[i].dl_acked        = 0;
            status = IPC_ST_OK;
            break;
        }
        break;
    }
    case IPC_REQ_REMOVE_DEVICE: {
        uint32_t dev_id;

        if (req->len < sizeof(dev_id)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&dev_id, req->payload, sizeof(dev_id));
        /* 1 removed, 0 nothing here; never held is not an error. */
        reply = (remove_device(dev_id) == 0) ? 1u : 0u;
        len = 1;
        break;
    }
    default:
        status = IPC_ST_UNKNOWN_REQ;
        break;
    }

    (void)ipc_send_reply(req, status, &reply, len);
}


/* The superframe that just closed: over, so its answer cannot still change.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void score_missed_reports(void) {
    uint32_t past = frame_counter - 1u;

    for (uint8_t i = 0; i < RADIO_MAX_DEVICES; i++) {
        dev_entry_t *d = &devices[i];

        if (!d->used || d->report_every == 0u)
            continue;
        /* Only an opportunity the grant actually named counts as a miss. */
        if ((past % d->report_every) != 0u)
            continue;
        if (d->frames_ok != 0u && d->last_superframe == past)
            d->missed_run = 0;
        else if (d->missed_run != 0xFFFFu)
            d->missed_run++;
    }
}

/* The counter keeps advancing through a quiesce; only transmission stops.
 * radio_devices_docs/open_hub/radio/superloop.md */
static void on_superframe(void) {
    /* Past what flash guarantees: silence, and the counter still advances.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (!kv_counter_safe(frame_counter)) {
        unreserved_frames++;
        return;
    }

    if (pair_state == RADIO_PAIR_QUIESCE) {
        int32_t left = (int32_t)(quiesce_resume_at - frame_counter);

        if (left > 0) {
            /* Repeated, counting down, so a missed copy still gives the same resume.
             * radio_devices_docs/radio/decisions/0020-device-triggered-quiesce.md */
            if ((uint32_t)(quiesce_len - left) < RADIO_QUIESCE_ANNOUNCE)
                RFM_send_broadcast(RADIO_BEACON_FLAG_QUIESCE, (uint8_t)left);
            else
                silent_frames++;
            return;
        }
        pair_state = pairing_open ? RADIO_PAIR_LISTEN : RADIO_PAIR_IDLE;
        quiesce_last_end = frame_counter;
        quiesce_len = 0;
    }

    if (quiesce_pending) {
        quiesce_pending = 0;
        /* The resume superframe is fixed here and never moved: devices sleep on it.
         * radio_devices_docs/radio/decisions/0020-device-triggered-quiesce.md */
        quiesce_resume_at = frame_counter + quiesce_len;
        pair_state = RADIO_PAIR_QUIESCE;
        RFM_send_broadcast(RADIO_BEACON_FLAG_QUIESCE, quiesce_len);
        return;
    }

    score_missed_reports();
    RFM_send_broadcast(0, 0);
}

/* --- the four-frame exchange ------------------------------------------- */

/* 1 when a frame is ready and its CRC verified; failures are counted and drained.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void rx_sample_rssi(void) {
    int16_t x2 = 0;

    if (rfm69_measure_rssi(phy_rfm69_dev(), RSSI_TIMEOUT_US, &x2) != RFM69_OK)
        return;
    rx_rssi_samples++;
    /* Half-dB below zero, returned negated: stronger is larger. */
    if (x2 > rx_rssi_peak_x2)  rx_rssi_peak_x2  = x2;   /* loudest */
    if (x2 < rx_rssi_floor_x2) rx_rssi_floor_x2 = x2;   /* quietest */
}

/* Only a level inside a request's payload sits below the sync word.
 * radio_devices_docs/radio/pairing.md */
static void join_sample_rssi(void) {
    uint32_t off_tk = rfm_micros() - superframe_start_tk;
    int16_t x2 = 0;
    int16_t *peak, *floor;

    if ((int32_t)(off_tk - (join_offset_tk + timebase_us_to_ticks(JP_MODE_US))) < 0 ||
        (int32_t)(off_tk - (join_offset_tk +
                            timebase_us_to_ticks(RADIO_TURN_INVITE_US))) > 0) {
        rx_sample_rssi();
        return;
    }
    /* Attempts, not successes: a trigger that never completes reads as a quiet band. */
    jp_level_tries++;
    if (rfm69_measure_rssi(phy_rfm69_dev(), RSSI_TIMEOUT_US, &x2) != RFM69_OK)
        return;
    rx_rssi_samples++;
    if (x2 > rx_rssi_peak_x2)  rx_rssi_peak_x2  = x2;
    if (x2 < rx_rssi_floor_x2) rx_rssi_floor_x2 = x2;
    peak  = (pi_last_sent_sf == frame_counter) ? &jp_inv_peak_x2  : &jp_idle_peak_x2;
    floor = (pi_last_sent_sf == frame_counter) ? &jp_inv_floor_x2 : &jp_idle_floor_x2;
    jp_levels++;
    if (x2 > *peak)  *peak  = x2;
    if (x2 < *floor) *floor = x2;
}

/* Folds one captured edge into the offset statistics.
 * radio_devices_docs/radio/phy-seam.md */
static void sync_edge_service(const phy_ev_t *ev) {
    uint32_t offset;

    if (!ev->sync_valid || ev->sync_seq == sync_seq_seen)
        return;
    sync_seq_seen = ev->sync_seq;
    sync_edges    = ev->sync_seq;
    sync_edge_tk  = ev->sync_us;
    sync_edge_base = radio_period_base(sync_edge_tk, superframe_start_tk,
                                       superframe_tk);

    /* TIM2 ticks, then converted: a tick is not a microsecond on this board. */
    sync_last_offset_tk = sync_edge_tk - sync_edge_base;
    sync_last_ppm       = calib_ppm();
    offset = timebase_ticks_to_us(sync_last_offset_tk);
    sync_last_offset_us  = offset;
    sync_last_superframe = frame_counter;

    /* Counted and reported raw, never filtered out. */
    if (offset >= SUPERFRAME_US * 2u) {
        sync_implausible++;
        return;
    }
    if (offset < sync_min_offset_us) sync_min_offset_us = offset;
    if (offset > sync_max_offset_us) sync_max_offset_us = offset;

    /* Unpaired is counted, never dropped: the sums must share one population. */
    if (bl_n == 0u || bl_sf != frame_counter) {
        sync_unpaired++;
        return;
    }
    if (!sync_ref_set) {
        sync_ref_us  = offset;
        sync_ref_set = 1;
    }
    {
        int32_t d = (int32_t)(offset - sync_ref_us);

        sync_stat_n++;
        sync_sum_d      += d;
        sync_sumsq_d    += (uint64_t)((int64_t)d * (int64_t)d);
        sync_lead_sum   += bl_last_us;
        sync_lead_sumsq += (uint64_t)bl_last_us * (uint64_t)bl_last_us;
        sync_cov_sum    += (int64_t)d * (int64_t)bl_last_us;
    }
}

/* Keyed after the sync word, and lag is measured from that same edge.
 * radio_devices_docs/open_hub/radio/configuration.md */
#define SYNC_RSSI_WINDOW_US  RADIO_AIR_SYNC_TO_END_US(RADIO_UPLINK_BYTES)
/* A span from the sync edge cannot be as long as the frame that contains it. */
_Static_assert(SYNC_RSSI_WINDOW_US < RADIO_AIR_START_TO_END_US(RADIO_UPLINK_BYTES),
               "the RSSI window is measured from the sync edge, not from frame start");

/* Which slot an edge landed in, off its offset. 0xFF is outside the region.
 * radio_devices_docs/radio/tdma.md */
static uint8_t slot_of_offset(uint32_t off) {
    uint32_t n;

    if (off < RADIO_UPLINK_OFFSET_US)
        return 0xFFu;
    n = (off - RADIO_UPLINK_OFFSET_US) / RADIO_SLOT_US;
    return (n < RADIO_SLOT_COUNT) ? (uint8_t)n : 0xFFu;
}

/* Taken while the payload still arrives: the only time the level is the frame's.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void sync_rssi_sample(const phy_ev_t *ev) {
    uint32_t lag;

    /* Attempts, not successes: never called and always failed read alike. */
    sync_rssi_taken++;
    /* 0 is the contract's "no level", and this receiver does not read 0 dBm. */
    if (ev->rssi_dbm == 0) {
        sync_rssi_err++;
        return;
    }
    lag = timebase_ticks_to_us(phy_now_us() - ev->sync_us);
    if (lag > 0xFFFFu)
        lag = 0xFFFFu;
    sync_rssi_dbm    = (int8_t)ev->rssi_dbm;
    sync_rssi_lag_us = (uint16_t)lag;
    /* Off this edge, not off the last one folded into the statistics. */
    sync_slot = slot_of_offset(timebase_ticks_to_us(ev->sync_us - sync_edge_base));
    /* 1 only inside the frame: outside it the sample is the floor under another name. */
    sync_rssi_have = (lag <= SYNC_RSSI_WINDOW_US) ? 1u : 0u;
    if (!sync_rssi_have)
        sync_rssi_late++;
    if (sync_rssi_lag_us > sync_rssi_lag_max_us)
        sync_rssi_lag_max_us = sync_rssi_lag_us;
}

/* A failed read is counted apart: it must not enter the fit as a zero. */
static void afc_note(uint8_t grid, const phy_ev_t *ev) {
    int32_t hz = ev->afc_hz;

    if (!ev->afc_valid) {
        afc_read_err++;
        return;
    }
    if (afc_n == 0u || hz < afc_min_hz) afc_min_hz = hz;
    if (afc_n == 0u || hz > afc_max_hz) afc_max_hz = hz;
    afc_last_hz   = hz;
    afc_last_grid = grid;
    afc_sum_hz   += hz;
    afc_sum_g    += (int64_t)grid;
    afc_sum_gg   += (int64_t)grid * (int64_t)grid;
    afc_sum_gh   += (int64_t)grid * (int64_t)hz;
    afc_ring_grid[afc_ring_head] = grid;
    afc_ring_afc_hz[afc_ring_head] = hz;
    afc_ring_gain[afc_ring_head] = ev->lna_gain;
    /* Consumed, never left behind: no frame may report the level of the one before. */
    afc_ring_rssi[afc_ring_head] = sync_rssi_have ? sync_rssi_dbm : 0;
    afc_ring_slot[afc_ring_head] = sync_rssi_have ? sync_slot : 0xFFu;
    if (sync_rssi_have)
        afc_ring_in_frame = (uint16_t)(afc_ring_in_frame | (1u << afc_ring_head));
    else
        afc_ring_in_frame = (uint16_t)(afc_ring_in_frame & ~(1u << afc_ring_head));
    sync_rssi_have = 0;
    afc_ring_crc_ok = (uint16_t)(afc_ring_crc_ok & ~(1u << afc_ring_head));
    afc_ring_head = (uint8_t)((afc_ring_head + 1u) % IPC_AFC_RING);
    afc_n++;
}

/* The outcome belongs to the entry afc_note just wrote, which is head - 1. */
static void afc_note_crc_ok(void) {
    uint8_t last = (uint8_t)((afc_ring_head + IPC_AFC_RING - 1u) % IPC_AFC_RING);

    afc_ring_crc_ok = (uint16_t)(afc_ring_crc_ok | (1u << last));
}

/* 1 when the event carried a whole frame; the failures are counted here too. */
static int rx_frame_ready(const phy_ev_t *ev, uint8_t grid) {
    if (ev->kind != PHY_EV_FRAME && ev->kind != PHY_EV_CRC)
        return 0;
    /* Before the CRC branch: the corrupt frames are the population AFC is for. */
    afc_note(grid, ev);
    if (ev->kind == PHY_EV_CRC) {
        rx_crc_err++;
        return 0;
    }
    rx_frames++;
    afc_note_crc_ok();
    return 1;
}

static void ex_reset(void) {
    ex_state   = RADIO_EX_IDLE;
    ex_due_frame = 0;
    ex_req_frame = 0;
    ex_dev_id  = 0;
    ex_waiting = 0;
    /* Dropped with the exchange it belongs to, so the next one cannot read it. */
    ex_reply_new = 0;
    ex_retry   = 0;
    memset(&ex_rsp, 0, sizeof(ex_rsp));
    memset(&ex_keys, 0, sizeof(ex_keys));
}

/* The channel is an argument, never a constant inside here.
 * radio_devices_docs/open_hub/radio/superloop.md */
static int frame_tx(const void *payload, uint8_t len, uint32_t hz) {
    if (phy_tune(hz) != 0)
        return -1;
    if (frame_send(payload, len) != 0)
        return -1;
    /* Back to listening immediately: standby loses the reply with no error. */
    (void)phy_listen();
    return 0;
}

/* Named for its channel, not its caller: every pairing frame goes out here. */
static int pair_tx(const void *payload, uint8_t len) {
    return frame_tx(payload, len, slot_hz(RADIO_JOIN_SLOT));
}

/* Split out so the self-test compares what transmits, not a second assembly.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
static void build_pair_rsp(radio_pair_rsp_t *f, uint32_t hid, uint32_t did,
                           const uint8_t eph[32], const uint8_t confirm[16]) {
    memset(f, 0, sizeof(*f));
    f->type    = RADIO_FRAME_PAIR_RSP;
    f->version = RADIO_PAIR_VERSION;
    f->hub_id  = hid;
    f->dev_id  = did;
    memcpy(f->eph_pubkey, eph, 32);
    memcpy(f->confirm, confirm, 16);
}

static void build_pair_accept_hdr(radio_pair_accept_t *f, uint32_t hid, uint32_t did,
                                  uint32_t superframe, uint8_t retry) {
    memset(f, 0, sizeof(*f));
    f->type       = RADIO_FRAME_PAIR_ACCEPT;
    f->version    = RADIO_PAIR_VERSION;
    f->hub_id     = hid;
    f->dev_id     = did;
    f->superframe = superframe;
    f->retry      = retry;
}

/* Every frame this core assembles, against bytes the far side also compiles.
 * radio_devices_docs/radio/crypto/wire-crypto.md */
static int frame_selftest(void) {
    radio_pair_rsp_t rsp;
    radio_pair_accept_t acc;

    /* Offsets from the struct: the points changed width and literals did not. */
    build_pair_rsp(&rsp, 0x33442211u, 0x0000002Au,
                   PV_FRAME_RSP + offsetof(radio_pair_rsp_t, eph_pubkey),
                   PV_FRAME_RSP + offsetof(radio_pair_rsp_t, confirm));
    if (memcmp(&rsp, PV_FRAME_RSP, sizeof(rsp)) != 0)
        return -1;

    /* Only the cleartext header, which is also the AAD. */
    build_pair_accept_hdr(&acc, 0x33442211u, 0x0000002Au,
                          0x1a2b3c4fu, 0x00u);
    if (memcmp(&acc, PV_ACCEPT_AAD, RADIO_PAIR_ACCEPT_AAD_LEN) != 0)
        return -2;
    return 0;
}

static void send_pair_rsp(void) {
    radio_pair_rsp_t f;

    build_pair_rsp(&f, hub_id, ex_dev_id, ex_rsp.eph_pubkey, ex_rsp.confirm);

    if (pair_tx(&f, (uint8_t)sizeof(f)) != 0) {
        ex_tx_err++;
        ex_reset();
        return;
    }
    ex_rsp_sent++;
    ex_state    = RADIO_EX_SENT_RSP;
    ex_deadline = rfm_micros() + timebase_us_to_ticks(EX_DEV_TIMEOUT_US);
}

/* The slot grant, sealed; the network hop key travels inside it.
 * radio_devices_docs/radio/crypto/key-lifecycle.md */
static void send_pair_accept(void) {
    radio_pair_accept_t f;
    radio_pair_grant_t grant;
    uint8_t nonce[AEAD_NONCE_BYTES];

    build_pair_accept_hdr(&f, hub_id, ex_dev_id, frame_counter, ex_retry);

    grant.slot         = ex_keys.slot;
    grant.report_every = ex_keys.report_every;
    grant.flags        = 0;
    memcpy(grant.hop_key, ex_keys.hop_key, sizeof(grant.hop_key));

    /* The nonce's slot field cannot be the slot being granted - it is sealed.
     * radio_devices_docs/radio/crypto/wire-crypto.md */
    aead_nonce(nonce, f.superframe, ex_dev_id, RADIO_DIR_DOWNLINK,
               RADIO_NONCE_SLOT_UNSLOTTED | ex_retry);

    if (aead_seal(ex_keys.session_key, nonce, (const uint8_t *)&f,
                  RADIO_PAIR_ACCEPT_AAD_LEN, (const uint8_t *)&grant,
                  sizeof(grant), f.ct, f.tag) != 0) {
        ex_seal_err++;
        ex_reset();
        return;
    }

    if (pair_tx(&f, (uint8_t)sizeof(f)) != 0) {
        ex_tx_err++;
        ex_reset();
        return;
    }
    ex_accepts_sent++;
    ex_state = RADIO_EX_ACCEPTED;
}

/* Installs or replaces one device; a re-pair keeps its slot and changes its key. */
static int install_device(const ipc_device_keys_t *k) {
    dev_entry_t *d;

    if (k->slot >= RADIO_MAX_DEVICES || k->dev_id == 0u)
        return -1;

    d = &devices[k->slot];
    if (!d->used)
        device_count++;
    memset(d, 0, sizeof(*d));
    /* Zero would read as an arrival on the boundary, which cannot happen. */
    d->arrival_sync_us = IPC_ARRIVAL_SYNC_NONE;
    d->used         = 1;
    d->slot         = k->slot;
    d->dev_id       = k->dev_id;
    d->key_gen      = k->key_gen;
    d->report_every = (k->report_every == 0u) ? 1u : k->report_every;
    memcpy(d->session_key, k->session_key, sizeof(d->session_key));

    /* Seed the replay floor from the durable counter, never from zero.
     * radio_devices_docs/open_hub/arch/keystore.md */
    d->rx_floor = frame_counter;
    d->rx_floor_slot = 0;

    /* A changed hop key invalidates the cached cycle, which is about a minute long.
     * radio_devices_docs/radio/hopping.md */
    if (!net_hop_key_set || memcmp(net_hop_key, k->hop_key, sizeof(net_hop_key)) != 0) {
        memcpy(net_hop_key, k->hop_key, sizeof(net_hop_key));
        net_hop_key_set = 1;
        if (hop_init(&hop, hop_prf_aes, NULL, RADIO_HOP_COUNT) != 0)
            return -1;
    }
    return 0;
}

/* The mirror of install_device: `used` was set in one place and cleared nowhere.
 * radio_devices_docs/open_hub/arch/ipc.md */
static int remove_device(uint32_t dev_id) {
    if (dev_id == 0u)
        return -1;
    for (uint8_t i = 0; i < RADIO_MAX_DEVICES; i++) {
        dev_entry_t *d = &devices[i];

        if (!d->used || d->dev_id != dev_id)
            continue;
        /* Zeroed the way install leaves it, so no half-removed entry exists. */
        memset(d, 0, sizeof(*d));
        d->arrival_sync_us = IPC_ARRIVAL_SYNC_NONE;
        if (device_count > 0u)
            device_count--;
        return 0;
    }
    return -1;
}

static void exchange_service(void) {
    ipc_msg_t reply;

    if (ex_state == RADIO_EX_IDLE || ex_state == RADIO_EX_ACCEPTED)
        return;

    /* evt_reply_service runs first in RFM_Routine, so this is the same pass. */
    if (ex_waiting && ex_reply_new) {
        reply = ex_reply;
        ex_reply_new = 0;
        ex_waiting = 0;
        if (reply.status != IPC_ST_OK) {
            /* CM7 refused; none of its reasons are worth a retry.
             * radio_devices_docs/radio/pairing.md */
            ex_cm7_refused++;
            ex_reset();
            return;
        }
        if (ex_state == RADIO_EX_WAIT_RSP) {
            if (reply.len < sizeof(ex_rsp)) { ex_cm7_refused++; ex_reset(); return; }
            memcpy(&ex_rsp, reply.payload, sizeof(ex_rsp));
            /* One turn per region; the derive had a whole superframe. ADR-0026 */
            ex_state     = RADIO_EX_RSP_DUE;
            ex_due_frame = ex_req_frame + 1u;
            ex_deadline  = rfm_micros() + timebase_us_to_ticks(EX_REGION_TIMEOUT_US);
            return;
        }
        if (ex_state == RADIO_EX_WAIT_KEYS) {
            if (reply.len < sizeof(ex_keys)) { ex_cm7_refused++; ex_reset(); return; }
            memcpy(&ex_keys, reply.payload, sizeof(ex_keys));
            /* Installed before the grant is transmitted, never after. */
            if (install_device(&ex_keys) != 0) { ex_reset(); return; }
            ex_paired++;
            /* Late is a frame nobody hears; the next region is one the device
             * still accepts. ADR-0026 */
            if (join_window_holds(RADIO_PAIR_ACCEPT_BYTES)) {
                send_pair_accept();
            } else {
                ex_state     = RADIO_EX_ACCEPT_DUE;
                ex_due_frame = frame_counter + 1u;
                ex_deferred++;
                ex_deadline  = rfm_micros() +
                               timebase_us_to_ticks(EX_REGION_TIMEOUT_US);
            }
            return;
        }
    }

    if (timebase_elapsed(ex_deadline)) {
        ex_timeouts++;
        ex_reset();
    }
}

/* --- the uplink region -------------------------------------------------- */

static void uplink_close(void) {
    if (!uplink_rx_open)
        return;
    (void)phy_standby();
    uplink_rx_open = 0;
}

static void handle_uplink_frame(const phy_ev_t *ev) {
    radio_uplink_t f;
    radio_uplink_report_t rpt;
    uint8_t nonce[AEAD_NONCE_BYTES];
    uint32_t dev_index;
    dev_entry_t *d;
    uint8_t len = ev->len;

    up_frames++;
    if (len < sizeof(f) || ev->buf[0] != RADIO_FRAME_UPLINK ||
        ev->buf[1] != RADIO_LINK_VERSION) {
        up_bad_frame++;
        return;
    }
    memcpy(&f, ev->buf, sizeof(f));

    /* No device id on the wire, and three slots map to one device.
     * radio_devices_docs/radio/tdma.md */
    if (f.slot >= RADIO_SLOT_COUNT) {
        up_bad_slot++;
        return;
    }
    dev_index = RADIO_SLOT_TO_DEVICE(f.slot);
    if (dev_index >= RADIO_MAX_DEVICES || !devices[dev_index].used) {
        up_bad_slot++;
        return;
    }
    d = &devices[dev_index];

    aead_nonce(nonce, f.superframe, d->dev_id, RADIO_DIR_UPLINK, f.slot);
    if (aead_open(d->session_key, nonce, (const uint8_t *)&f, RADIO_UPLINK_AAD_LEN,
                  f.ct, sizeof(f.ct), (uint8_t *)&rpt, f.tag) != 0) {
        up_bad_tag++;
        d->frames_bad++;
        return;
    }

    /* After the tag, never before. The floor is a tuple; signed for the wrap.
     * radio_devices_docs/open_hub/arch/keystore.md */
    {
        int32_t age = (int32_t)(f.superframe - d->rx_floor);

        if (age < 0 || (age == 0 && f.slot <= d->rx_floor_slot)) {
            up_replay++;
            d->frames_replay++;
            return;
        }
    }
    d->rx_floor      = f.superframe;
    d->rx_floor_slot = f.slot;

    up_ok++;
    d->frames_ok++;
    /* Cycles, not frames. The minimum is the cadence, the mean carries loss. */
    if (f.superframe != d->cyc_last_sf) {
        if (d->cyc_last_sf != 0u) {
            uint32_t gap = f.superframe - d->cyc_last_sf;

            if (gap <= 0xFFFFu) {
                if (d->cyc_n == 0u || gap < d->cyc_min)
                    d->cyc_min = (uint16_t)gap;
                d->cyc_sum += gap;
                d->cyc_n++;
            }
        }
        d->cyc_last_sf = f.superframe;
    }
    d->last_superframe = f.superframe;
    /* The echo names the command, so a stale ack cannot pass for this one. */
    if (!d->dl_acked && d->dl_cmd_seq != RADIO_CMD_SEQ_NONE &&
        rpt.ack_seq == d->dl_cmd_seq && rpt.ack_cmd == d->dl_cmd) {
        d->dl_acked   = 1;
        d->dl_repeats = 0;
        /* Recorded, never compared until the device fills it in. ROADMAP item 32 */
        d->dl_ack_arg = rpt.ack_arg;
        dl_cmd_acked++;
    }
    d->arrival_us = timebase_ticks_to_us(rfm_micros() - superframe_start_tk);
    /* The edge is global, so it is paired to this frame or the field stays absent.
     * radio_devices_docs/open_hub/radio/sync-timestamp.md */
    {
        uint32_t since = timebase_ticks_to_us(rfm_micros() - sync_edge_tk);
        uint32_t air   = RADIO_AIR_SYNC_TO_END_US(RADIO_UPLINK_BYTES);

        if (since < air || since > air + RADIO_SYNC_PAIR_SLACK_US) {
            d->arrival_sync_us = IPC_ARRIVAL_SYNC_NONE;
            up_sync_unpaired++;
            /* Per device as well: the hub-wide one cannot reach an event, which
             * carries one device. radio_devices_docs/open_hub/network/telemetry.md */
            if (d->sync_unpaired != 0xFFFFu)
                d->sync_unpaired++;
        } else {
            d->arrival_sync_us =
                timebase_ticks_to_us(sync_edge_tk - sync_edge_base);
        }
    }
    d->rssi_up   = (int8_t)ev->rssi_dbm;
    /* Cleared here too: a frame in its own superframe outruns the score. */
    d->missed_run = 0;
    d->rssi_down = rpt.rssi_down;
    d->flags     = rpt.flags;
    d->supply_mv = rpt.supply_mv;
    d->temp_c_x10 = rpt.temp_c_x10;
    d->uptime_s  = rpt.uptime_s;
    uplink_notify(d);
}

/* Whether this many bytes still fit the window. ADR-0026 */
static int join_window_holds(uint8_t payload_b) {
    uint32_t off, need;

    if (!grid_started)
        return 0;
    off  = timebase_ticks_to_us(rfm_micros() - superframe_start_tk);
    need = RADIO_AIR_START_TO_END_US(payload_b);
    if (off < RADIO_JOIN_OFFSET_US)
        return 0;
    return (off - RADIO_JOIN_OFFSET_US) + need < RADIO_JOIN_RX_US;
}

/* 1 while a staged turn is due in this region. */
static int pair_turn_due(void) {
    return (ex_state == RADIO_EX_RSP_DUE || ex_state == RADIO_EX_ACCEPT_DUE) &&
           (int32_t)(frame_counter - ex_due_frame) >= 0;
}

/* 1 while the exchange owns this region, transmitting in it or listening in it.
 * radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md */
static int pair_region_owned(void) {
    if (ex_state == RADIO_EX_IDLE || ex_state == RADIO_EX_ACCEPTED)
        return 0;
    return (uint32_t)(frame_counter - ex_req_frame) < RADIO_PAIR_REGIONS;
}

/* The staged turn, at the offset a joining device listens on. ADR-0026 */
static void pair_turn_service(void) {
    if (!pair_turn_due() || !grid_started)
        return;
    if (!timebase_elapsed(superframe_start_tk + join_offset_tk))
        return;
    if (ex_state == RADIO_EX_RSP_DUE)
        send_pair_rsp();
    else
        send_pair_accept();
}

/* Keyed on the join channel at the superframe CM7 named. ADR-0021 */
static void pair_init_service(void) {
    if (pi_superframe == 0u || pi_len == 0u)
        return;
    if (!grid_started || pair_state != RADIO_PAIR_LISTEN)
        return;
    /* An exchange in flight owns its regions; a new invitation waits. ADR-0026 */
    if (pair_region_owned())
        return;

    if ((int32_t)(frame_counter - pi_superframe) > 0) {
        pi_missed++;
        pi_superframe = 0u;
        pi_len = 0u;
        return;
    }
    if (frame_counter != pi_superframe)
        return;
    /* The offset the join beacon has always used, where a device is listening. */
    if (!timebase_elapsed(superframe_start_tk + join_offset_tk))
        return;

    pi_superframe = 0u;
    if (frame_tx(pi_frame, pi_len, slot_hz(RADIO_JOIN_SLOT)) != 0)
        pi_tx_err++;
    else {
        pi_sent++;
        pi_last_sent_sf = frame_counter;
    }
    /* Read back off the part, since PacketSent is not evidence about the carrier.
     * radio_devices_docs/open_hub/radio/configuration.md */
    {
        uint8_t v = 0;

        pi_frf = 0;
        if (rfm69_read_reg(phy_rfm69_dev(), RFM69_RegFrfMsb, &v) == RFM69_OK)
            pi_frf |= (uint32_t)v << 16;
        if (rfm69_read_reg(phy_rfm69_dev(), RFM69_RegFrfMid, &v) == RFM69_OK)
            pi_frf |= (uint32_t)v << 8;
        if (rfm69_read_reg(phy_rfm69_dev(), RFM69_RegFrfLsb, &v) == RFM69_OK)
            pi_frf |= v;
        if (rfm69_read_reg(phy_rfm69_dev(), RFM69_RegPayloadLength, &v) == RFM69_OK)
            pi_paylen = v;
    }
    pi_len = 0u;
}

/* The hub's half of the periodic exchange: downlink region, hop channel, half rate,
 * sealed. radio_devices_docs/open_hub/radio/superloop.md */
static void downlink_service(void) {
    radio_downlink_t f;
    radio_downlink_cmd_t body;
    uint8_t nonce[AEAD_NONCE_BYTES];
    dev_entry_t *d = NULL;
    uint8_t i, slot, hop_idx;
    uint8_t carried = 0;

    if (!grid_started || pair_state == RADIO_PAIR_QUIESCE || device_count == 0u)
        return;
    if (!RADIO_DOWNLINK_ON(frame_counter))
        return;
    if (dl_served == frame_counter)
        return;
    if (!timebase_elapsed(superframe_start_tk +
                          timebase_us_to_ticks(RADIO_DOWNLINK_RX_OPEN_US)))
        return;
    /* Past the region is not late but a different region: uplink slot 0 opens there. */
    if (timebase_elapsed(superframe_start_tk +
                         timebase_us_to_ticks(RADIO_DOWNLINK_RX_CLOSE_US)))
        return;

    dl_served = frame_counter;
    dl_opportunities++;

    /* Round robin, scanned rather than indexed because slots are sparse.
     * radio_devices_docs/open_hub/radio/superloop.md */
    for (i = 0; i < RADIO_MAX_DEVICES; i++) {
        slot = (uint8_t)((dl_next_slot + i) % RADIO_MAX_DEVICES);
        if (devices[slot].used) {
            d = &devices[slot];
            dl_next_slot = (uint8_t)((slot + 1u) % RADIO_MAX_DEVICES);
            break;
        }
    }
    if (d == NULL) {
        dl_no_device++;
        return;
    }

    memset(&body, 0, sizeof(body));
    /* A queued command rides this downlink; the repeat is spent only if it flies. */
    if (d->dl_repeats > 0u) {
        body.cmd          = d->dl_cmd;
        body.report_every = d->dl_report_every;
        body.arg          = d->dl_arg;
        body.cmd_seq      = d->dl_cmd_seq;
        carried = 1;
    } else {
        body.cmd = RADIO_CMD_NOP;
    }
    /* SUPERFRAME_US of nominal time, so seconds is a multiply, not a divide. */
    body.hub_time_s = frame_counter * (SUPERFRAME_US / 1000000u);

    if (!dl_nonce_is_new(d, frame_counter)) {
        dl_nonce_refused++;
        return;
    }

    memset(&f, 0, sizeof(f));
    f.type       = RADIO_FRAME_DOWNLINK;
    f.version    = RADIO_LINK_VERSION;
    f.slot       = d->slot;
    f.superframe = frame_counter;

    /* Spent at the cipher, not at its success: a retry would be a second body. */
    d->dl_nonce_sf   = frame_counter;
    d->dl_nonce_used = 1;

    aead_nonce(nonce, f.superframe, d->dev_id, RADIO_DIR_DOWNLINK, f.slot);
    if (aead_seal(d->session_key, nonce, (const uint8_t *)&f, RADIO_DOWNLINK_AAD_LEN,
                  (const uint8_t *)&body, sizeof(body), f.ct, f.tag) != 0) {
        dl_seal_err++;
        return;
    }
    /* Computed here, never inherited from what the beacon left tuned.
     * radio_devices_docs/open_hub/radio/superloop.md */
    if (hop_channel(&hop, frame_counter, &hop_idx) != 0) {
        dl_prf_err++;
        return;
    }
    dl_last_hz = slot_hz(hop_slot_to_grid(hop_idx));
    dl_last_sf = frame_counter;
    if (frame_tx(&f, (uint8_t)sizeof(f), dl_last_hz) != 0) {
        dl_tx_err++;
        return;
    }
    dl_sent++;
    if (carried) {
        d->dl_repeats--;
        dl_cmd_sent++;
        /* The last repeat spent with no echo: counted, because silence is not proof. */
        if (d->dl_repeats == 0u && !d->dl_acked)
            dl_cmd_lost++;
    }
}

/* Open for the whole uplink region, closed before the join region and the boundary.
 * radio_devices_docs/open_hub/radio/superloop.md */
static void uplink_service(void) {
    uint32_t close_at;

    if (pair_state == RADIO_PAIR_QUIESCE || !grid_started || device_count == 0u) {
        uplink_close();
        return;
    }

    /* The join region owns the tail while a window is open, so this ends first. */
    close_at = superframe_start_tk +
        ((pair_state == RADIO_PAIR_LISTEN) ? join_offset_tk
         : superframe_tk - timebase_us_to_ticks(RADIO_END_GUARD_US));

    if (!timebase_elapsed(superframe_start_tk +
                          timebase_us_to_ticks(RADIO_UPLINK_OFFSET_US))) {
        uplink_close();
        return;
    }
    if (timebase_elapsed(close_at)) {
        uplink_close();
        return;
    }

    if (!uplink_rx_open) {
        /* Tuned explicitly rather than inherited from the beacon.
         * radio_devices_docs/open_hub/radio/superloop.md */
        uint8_t idx;

        if (hop_channel(&hop, frame_counter, &idx) != 0)
            return;
        up_grid = (uint8_t)hop_slot_to_grid(idx);
        if (phy_tune(slot_hz(up_grid)) != 0)
            return;
        if (phy_listen() != 0)
            return;
        uplink_rx_open = 1;
        up_windows++;
        return;
    }

    {
        phy_ev_t ev;

        if (phy_poll(&ev) != 0) {
            rx_flushes++;
            return;
        }
        sync_edge_service(&ev);
        if (ev.kind == PHY_EV_SYNC) {
            rx_sync_match++;
            /* Counted apart from the join channel's sync, which pairing traffic moves. */
            up_sync++;
            sync_rssi_sample(&ev);
        }
        if (rx_frame_ready(&ev, up_grid))
            handle_uplink_frame(&ev);
        /* After the frame and only with sync clear: the latch has one reader.
         * radio_devices_docs/open_hub/radio/configuration.md */
        else if (!ev.busy) {
            int16_t x2 = 0;

            if (rfm69_measure_rssi(phy_rfm69_dev(), RSSI_TIMEOUT_US, &x2) == RFM69_OK) {
                if (x2 > up_rssi_peak_x2)  up_rssi_peak_x2  = x2;
                if (x2 < up_rssi_floor_x2) up_rssi_floor_x2 = x2;
            }
        }
    }
}

static void handle_join_frame(const phy_ev_t *ev) {
    uint8_t len = ev->len;

    /* Recorded before any check runs: what the frame was, not why it was refused. */
    rx_last_len        = len;
    rx_last_type       = ev->buf[0];
    rx_last_rssi       = (int8_t)ev->rssi_dbm;
    rx_last_superframe = frame_counter;

    if (len < 2u || ev->buf[1] != RADIO_PAIR_VERSION) {
        pair_reqs_dropped++;
        reqs_drop_last = RADIO_DROP_VERSION;
        return;
    }

    if (ev->buf[0] == RADIO_FRAME_PAIR_CONF) {
        radio_pair_conf_t c;
        ipc_pair_conf_evt_t e;

        /* Only while waiting, and only from the device the exchange belongs to. */
        if (ex_state != RADIO_EX_SENT_RSP || len < sizeof(c)) {
            pair_reqs_dropped++;
            reqs_drop_last = (len < sizeof(c)) ? RADIO_DROP_LEN : RADIO_DROP_BUSY;
            return;
        }
        memcpy(&c, ev->buf, sizeof(c));
        if (c.hub_id != hub_id || c.dev_id != ex_dev_id) {
            pair_reqs_dropped++;
            reqs_drop_last = (c.hub_id != hub_id) ? RADIO_DROP_HUB_ID
                                                  : RADIO_DROP_DEV_ID;
            reqs_drop_hub = c.hub_id;
            reqs_drop_dev = c.dev_id;
            return;
        }

        e.dev_id = c.dev_id;
        memcpy(e.confirm, c.confirm, sizeof(e.confirm));
        if (ipc_send_event(IPC_EVT_PAIR_CONF, &e, (uint8_t)sizeof(e), &ex_seq) != 0) {
            ex_reset();
            return;
        }
        ex_confs_forwarded++;
        ex_waiting  = 1;
        ex_state    = RADIO_EX_WAIT_KEYS;
        ex_deadline = rfm_micros() + timebase_us_to_ticks(EX_CM7_TIMEOUT_US);
        return;
    }

    if (ev->buf[0] != RADIO_FRAME_PAIR_REQ) {
        pair_reqs_dropped++;
        reqs_drop_last = RADIO_DROP_TYPE;
        return;
    }

    {
        radio_pair_req_t req;
        ipc_pair_req_evt_t e;

        pair_reqs_seen++;
        if (len < sizeof(req)) {
            /* A length this side does not compile: a wire-contract mismatch. */
            pair_reqs_dropped++;
            reqs_drop_last = RADIO_DROP_LEN;
            return;
        }
        /* Before anything judges it, while the bytes are still the radio's. */
        memcpy(reqs_drop_head, ev->buf, sizeof(reqs_drop_head));
        memcpy(reqs_drop_key,  ev->buf + 24, sizeof(reqs_drop_key));
        memcpy(&req, ev->buf, sizeof(req));

        /* Only the device the operator named, and each refusal counted apart.
         * radio_devices_docs/radio/joining.md */
        if (!pairing_open) {
            pair_reqs_dropped++;
            reqs_drop_last = RADIO_DROP_NO_WINDOW;
            return;
        }
        if (req.net_id != RADIO_NET_ID || req.hub_id != hub_id ||
            req.dev_id != pairing_dev_id) {
            pair_reqs_dropped++;
            reqs_drop_last = (req.net_id != RADIO_NET_ID) ? RADIO_DROP_NET_ID
                           : (req.hub_id != hub_id)       ? RADIO_DROP_HUB_ID
                                                          : RADIO_DROP_DEV_ID;
            reqs_drop_net = req.net_id;
            reqs_drop_hub = req.hub_id;
            reqs_drop_dev = req.dev_id;
            return;
        }
        /* One exchange at a time; the first device has committed and is waiting. */
        if (ex_state != RADIO_EX_IDLE && ex_state != RADIO_EX_ACCEPTED) {
            pair_reqs_dropped++;
            reqs_drop_last = RADIO_DROP_BUSY;
            return;
        }

        /* No quiesce: every turn is inside the region the schedule reserves.
         * ADR-0026 */

        e.dev_id     = req.dev_id;
        e.superframe = req.superframe;
        memcpy(e.dev_nonce, req.dev_nonce, sizeof(e.dev_nonce));
        memcpy(e.pubkey, req.pubkey, sizeof(e.pubkey));

        if (ipc_send_event(IPC_EVT_PAIR_REQ, &e, (uint8_t)sizeof(e), &ex_seq) != 0) {
            ex_reset();
            return;
        }
        ex_reqs_forwarded++;
        ex_dev_id    = req.dev_id;
        ex_waiting   = 1;
        ex_state     = RADIO_EX_WAIT_RSP;
        ex_retry     = 0;
        ex_req_frame = frame_counter;
        ex_deadline  = rfm_micros() + timebase_us_to_ticks(EX_CM7_TIMEOUT_US);
    }
}

/* Overlays the uplink tail, and split across superloop passes.
 * radio_devices_docs/open_hub/radio/superloop.md */
static void join_region_service(void) {
    if (pair_state == RADIO_PAIR_IDLE) {
        if (join_phase) {
            (void)phy_standby();
            join_phase = 0;
        }
        return;
    }

    if (join_phase == 0u) {
        /* With nothing paired, listen across the whole superframe.
         * radio_devices_docs/open_hub/radio/superloop.md */
        uint32_t open_tk = (pair_state == RADIO_PAIR_QUIESCE) ? 0u
                         : (device_count == 0u) ? timebase_us_to_ticks(RADIO_UPLINK_OFFSET_US)
                         : join_offset_tk;

        if (!grid_started || join_served_frame == frame_counter)
            return;
        if (!timebase_elapsed(superframe_start_tk + open_tk))
            return;
        join_served_frame = frame_counter;
        join_regions++;

        /* The handoff is explicit, not left to the order of two superloop calls.
         * radio_devices_docs/open_hub/radio/superloop.md */
        uplink_close();

        /* Half rate keeps the extra air time to 0.21%, and the beacon keeps its
         * documented offset. radio_devices_docs/radio/joining.md */
        if (device_count == 0u && pair_state == RADIO_PAIR_LISTEN) {
            if (phy_tune(slot_hz(RADIO_JOIN_SLOT)) != 0)
                return;
            join_beacon_pending = ((frame_counter % JOIN_BEACON_EVERY) == 0u);
        } else if ((frame_counter % JOIN_BEACON_EVERY) == 0u &&
                   pi_superframe != frame_counter && !pair_region_owned()) {
            RFM_send_join_beacon();
        } else if (phy_tune(slot_hz(RADIO_JOIN_SLOT)) != 0) {
            return;
        }

        if (phy_listen() != 0)
            return;
        /* During a quiesce the window runs to the boundary, stopping short of it. */
        join_rx_deadline = (pair_state == RADIO_PAIR_QUIESCE || device_count == 0u)
            ? superframe_start_tk + superframe_tk
              - timebase_us_to_ticks(RADIO_END_GUARD_US)
            : rfm_micros() + timebase_us_to_ticks(RADIO_JOIN_RX_US);
        join_phase = 1;
        jp_windows++;
        jp_step = 0;
        return;
    }

    jp_passes++;

    if (join_beacon_pending && pair_region_owned())
        join_beacon_pending = 0;        /* the region belongs to the exchange */

    if (join_beacon_pending &&
        timebase_elapsed(superframe_start_tk + join_offset_tk)) {
        join_beacon_pending = 0;
        /* Out of RX first.
         * radio_devices_docs/open_hub/radio/configuration.md */
        (void)phy_standby();
        RFM_send_join_beacon();
        (void)phy_listen();   /* straight back to listening */
    }

    {
        phy_ev_t ev;

        if (phy_poll(&ev) != 0)
            rx_flushes++;
        else {
            sync_edge_service(&ev);
            if (ev.kind == PHY_EV_SYNC) {
                rx_sync_match++;
                sync_rssi_sample(&ev);
            }
            if (rx_frame_ready(&ev, RADIO_JOIN_SLOT))
                handle_join_frame(&ev);
            /* After the frame, never before: a trigger overwrites the arriving level. */
            else if (!ev.busy)
                join_sample_rssi();
        }
    }

    /* Read off the part: the driver's shadow cannot show a set_mode that did not
     * take. radio_devices_docs/radio/pairing.md */
    if (jp_step == 0u &&
        timebase_elapsed(superframe_start_tk + join_offset_tk +
                         timebase_us_to_ticks(JP_MODE_US))) {
        uint8_t op = 0;

        jp_step = 1;
        if (rfm69_read_reg(phy_rfm69_dev(), RFM69_RegOpMode, &op) == RFM69_OK) {
            uint8_t off = (((op >> 2) & 0x07u) != (uint8_t)RFM69_MODE_RX) ? 1u : 0u;

            jp_last_op = op;
            jp_probes++;
            jp_not_rx += off;
            if (pi_last_sent_sf == frame_counter) {
                jp_inv_probes++;
                jp_inv_not_rx += off;
            }
        }
    }

    if (timebase_elapsed(join_rx_deadline)) {
        (void)phy_standby();
        join_beacon_pending = 0;
        join_phase = 0;
    }
}

/* 1 only when this call armed one, so a caller cannot count attempts as quiesces. */
static uint8_t begin_quiesce(uint8_t superframes) {
    if (pair_state == RADIO_PAIR_QUIESCE || quiesce_pending)
        return 0;
    if ((int32_t)(frame_counter - quiesce_last_end) < (int32_t)RADIO_QUIESCE_MIN_GAP) {
        quiesce_refused++;
        return 0;
    }
    if (superframes == 0u || superframes > RADIO_QUIESCE_SUPERFRAMES)
        superframes = RADIO_QUIESCE_SUPERFRAMES;
    quiesce_len = superframes;
    quiesce_pending = 1;
    return 1;
}

void RFM_Routine(void) {
    ipc_msg_t request;

    if (pairing_open && timebase_elapsed(pairing_deadline_us)) {
        pairing_open = 0;
        if (pair_state == RADIO_PAIR_LISTEN)
            pair_state = RADIO_PAIR_IDLE;
    }

    evt_reply_service();

    if (superframe_due())
        on_superframe();

    join_region_service();
    /* Before the invitation: a staged turn owns the region it was scheduled in. */
    pair_turn_service();
    pair_init_service();
    exchange_service();
    downlink_service();
    uplink_service();

    /* First half only: a flash program stalls this core for nearly a millisecond.
     * radio_devices_docs/open_hub/radio/superloop.md */
    if (grid_started && !timebase_elapsed(superframe_start_tk + superframe_tk / 2u))
        (void)kv_reserve(frame_counter);

    /* Drained by polling; the flag is cleared only so it does not stay pending.
     * radio_devices_docs/open_hub/arch/ipc.md */
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M7_TO_M4_RFM));
    while (ipc_poll_request(&request))
        RFM_serve_request(&request);
}

/* Resets the below-frame counters, which are cumulative since boot.
 * radio_devices_docs/open_hub/radio/superloop.md */
static void rx_diag_reset(void) {
    rx_sync_match = 0;
    rx_frames = 0;
    rx_crc_err = 0;
    rx_flushes = 0;
    rx_rssi_peak_x2 = -32768;
    rx_rssi_floor_x2 = 32767;
    rx_rssi_samples = 0;
    jp_windows = 0;
    jp_passes = 0;
    jp_probes = 0;
    jp_not_rx = 0;
    jp_inv_probes = 0;
    jp_inv_not_rx = 0;
    jp_levels = 0;
    jp_level_tries = 0;
    jp_inv_peak_x2 = -32768;
    jp_inv_floor_x2 = 32767;
    jp_idle_peak_x2 = -32768;
    jp_idle_floor_x2 = 32767;
    jp_last_op = 0;
    rx_last_len = 0;
    rx_last_type = 0;
    rx_last_rssi = 0;
    rx_last_superframe = 0;
    sync_rssi_lag_max_us = 0;
    sync_rssi_taken = 0;
    sync_rssi_late = 0;
    sync_rssi_err = 0;
}

static uint8_t RFM_open_pairing(uint32_t dev_id) {
    rx_diag_reset();
    pairing_dev_id = dev_id;
    pairing_deadline_us = rfm_micros() + timebase_us_to_ticks(PAIRING_WINDOW_MS * 1000u);
    pairing_open = 1;
    if (pair_state == RADIO_PAIR_IDLE)
        pair_state = RADIO_PAIR_LISTEN;
    return 0;
}
