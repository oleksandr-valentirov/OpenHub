/**
 * @file telemetry.c
 * @brief The northbound link: telemetry out, commands in, over one TCP socket.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */
#include <string.h>
#include <stdio.h>

#include "cmsis_os.h"
#include "FreeRTOS.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "telemetry.h"
#include "build_id.h"
/* Truncation would emit a plausible id; the build fails instead. */
_Static_assert(sizeof(BUILD_ID) <= 32u, "BUILD_ID does not fit oht_hello_t.build");
#include "oht_proto.h"
#include "hubipc.h"
#include "keystore.h"
#include "cfgstoreapi.h"
#include "hubconfig.h"
#include "pairing.h"
#include "radio_protocol.h"
#include "radio_layout.h"
#include "rng.h"
#include "hop_v1.h"
#include "pair_v4.h"
#include "main.h"

/* Sized to leave the whole body inside one segment, not to the protocol cap. */
#define TX_BUF_LEN        1400u
#define RX_BUF_LEN        320u
#define DEVICES_PER_FRAME 6u
#define RECONNECT_MS      3000u
#define POLL_MS           50u
#define SNAPSHOT_MS_MIN   200u
#define SNAPSHOT_MS_DEF   5000u
#define UPLINK_QUEUE_LEN  4u
#define CONNECT_TIMEOUT_MS 4000u

/* A server that has said nothing for this long is gone whatever TCP believes. */
#define IDLE_TIMEOUT_MS   30000u
/* The hub asks rather than waiting to be asked. radio_devices_docs/open_hub/network/telemetry.md */
#define KEEPALIVE_MS      10000u

static struct {
    char     ip[16];
    uint16_t port;
    uint8_t  token[TELEMETRY_TOKEN_MAX];
    uint8_t  token_len;
} cfg;

static telemetry_stats_t stats;
static osMessageQueueId_t uplink_q;
static int      sock = -1;
static uint16_t tx_seq;
static uint32_t boot_id;
static uint32_t session_start_ms;
static uint32_t last_rx_ms;
static uint8_t  tx_buf[TX_BUF_LEN];
static uint8_t  rx_buf[RX_BUF_LEN];
static volatile uint8_t snapshot_now;

/* Everything one snapshot needs, fetched once so the frame is self consistent. */
typedef struct snap {
    ipc_timing_t        timing;
    ipc_exchange_state_t ex;
    ipc_rx_diag_t       rx;
    ipc_downlink_state_t dl;
    ipc_evt_latency_t   lat;
    ipc_pair_state_t    pair;
    ipc_afc_raw_t       afc;
    uint8_t             have_timing;
    uint8_t             have_ex;
    uint8_t             have_rx;
    uint8_t             have_dl;
    uint8_t             have_lat;
    uint8_t             have_pair;
    uint8_t             have_afc;
} snap_t;

static ipc_msg_t reply;

/* A failed fetch leaves its `have` clear: the field is absent, not zero. */
static uint8_t fetch(uint8_t type, uint8_t arg, void *out, uint8_t len) {
    if (hub_ipc_call(type, arg, NULL, 0, &reply) != IPC_ST_OK)
        return 0;
    if (reply.len < len)
        return 0;
    memcpy(out, reply.payload, len);
    return 1;
}

/* ---- the socket ------------------------------------------------------- */

static void disconnect(uint8_t reason) {
    if (sock >= 0) {
        lwip_close(sock);
        sock = -1;
    }
    if (stats.connected) {
        stats.connected = 0;
        stats.disconnects++;
    }
    stats.last_disc_reason = reason;
    stats.up_ms = 0;
}

static int send_all(const uint8_t *p, uint16_t len) {
    uint16_t sent = 0;

    while (sent < len) {
        int n = lwip_send(sock, &p[sent], (size_t)(len - sent), 0);

        if (n <= 0)
            return -1;
        sent = (uint16_t)(sent + n);
    }
    return 0;
}

/**
 * @brief Writes one frame. Any refusal drops the link rather than being retried.
 * @param type     OHT_MSG_*
 * @param payload  the body, or NULL
 * @param len      its length
 * @param seq      the sequence number, or 0 to draw the next
 * @param flags    an OHT_EVT_* on an event, otherwise 0
 * @retval 0  written
 * @retval -1 the socket refused it; the caller must stop using the link
 */
static int send_frame(uint8_t type, const uint8_t *payload, uint16_t len,
                      uint16_t seq, uint16_t flags) {
    oht_hdr_t h;

    if (sock < 0)
        return -1;
    if (seq == 0u) {
        tx_seq = (uint16_t)(tx_seq + 1u);
        if (tx_seq == 0u)
            tx_seq = 1u;
        seq = tx_seq;
    }
    h.ver   = OHT_VERSION;
    h.type  = type;
    h.len   = len;
    h.seq   = seq;
    h.flags = flags;

    if (send_all((const uint8_t *)&h, sizeof(h)) != 0 ||
        (len != 0u && send_all(payload, len) != 0)) {
        stats.tx_fail++;
        return -1;
    }
    stats.frames_tx++;
    stats.bytes_tx += (uint32_t)sizeof(h) + len;
    return 0;
}

/**
 * @brief Waits for the socket to become readable.
 * @param ms  how long to wait
 * @retval 1  there is something to read
 * @retval 0  the wait expired
 * @retval -1 the socket failed
 *
 * This is what makes the read a poll. SO_RCVTIMEO does not: LWIP_SO_RCVTIMEO
 * defaults to 0 and is not enabled in lwipopts.h, so setting it is accepted in
 * silence and recv blocks forever. That cost this link its whole cadence once -
 * snapshots came at the server's ping interval and nothing said so.
 * radio_devices_docs/open_hub/network/telemetry.md
 */
static int wait_readable(uint32_t ms) {
    struct timeval tv;
    fd_set rd;
    int rc;

    if (sock < 0)
        return -1;
    FD_ZERO(&rd);
    FD_SET(sock, &rd);
    tv.tv_sec  = (long)(ms / 1000u);
    tv.tv_usec = (long)((ms % 1000u) * 1000u);
    rc = lwip_select(sock + 1, &rd, NULL, NULL, &tv);
    if (rc < 0)
        return -1;
    return (rc > 0) ? 1 : 0;
}

/* Reads exactly n bytes or gives up; a short read is a torn frame, not a gap. */
static int recv_exact(uint8_t *p, uint16_t n, uint32_t ms) {
    uint16_t got = 0;

    while (got < n) {
        int r = wait_readable(ms);

        if (r < 0)
            return -1;
        if (r == 0)
            return (got == 0u) ? 1 : -1;
        r = lwip_recv(sock, &p[got], (size_t)(n - got), 0);
        if (r <= 0)
            return -1;
        got = (uint16_t)(got + r);
    }
    return 0;
}

/* ---- the bodies -------------------------------------------------------- */

static void put_hub(oht_writer_t *w, const snap_t *s) {
    oht_rec_begin(w, OHT_OBJ_HUB, 0);
    OHT_PUT(w, OHT_F_HUB_UPTIME_MS, osKernelGetTickCount());
    OHT_PUT(w, OHT_F_HUB_IPC_READY, ipc_ready());
    /* The threshold a reader needs to make sense of every device's missed_run. */
    OHT_PUT(w, OHT_F_HUB_LINK_LOST_MISSES, hubconfig_link_lost());
    OHT_PUT(w, OHT_F_HUB_IPC_STALE_REPLIES, ipc_stale_replies());

    if (s->have_timing) {
        OHT_PUT(w, OHT_F_HUB_SUPERFRAME, s->timing.superframe);
        OHT_PUT(w, OHT_F_HUB_PERIOD_US, s->timing.period_us);
        OHT_PUT(w, OHT_F_HUB_CALIB_PPM, s->timing.calib_ppm);
        OHT_PUT(w, OHT_F_HUB_LATE_LAST_US, s->timing.late_last_us);
        OHT_PUT(w, OHT_F_HUB_LATE_MAX_US, s->timing.late_max_us);
        OHT_PUT(w, OHT_F_HUB_LATE_OVER, s->timing.late_over);
        OHT_PUT(w, OHT_F_HUB_CALIB_WINDOWS, s->timing.calib_windows);
        OHT_PUT(w, OHT_F_HUB_CALIB_REJECTS, s->timing.calib_rejects);
        /* Counted over passes, not arrivals - the pair that lag_max_us cannot
         * make. ROADMAP item 37 */
        OHT_PUT(w, OHT_F_HUB_LOOP_LAST_US, s->timing.loop_last_us);
        OHT_PUT(w, OHT_F_HUB_LOOP_MAX_US, s->timing.loop_max_us);
        OHT_PUT(w, OHT_F_HUB_LOOP_PASSES, s->timing.loop_passes);
    }
    if (s->have_ex) {
        OHT_PUT(w, OHT_F_HUB_DEVICES, s->ex.devices);
        OHT_PUT(w, OHT_F_HUB_PAIRED_TOTAL, s->ex.paired);
        OHT_PUT(w, OHT_F_HUB_REPORT_EVERY, s->ex.report_every);
        OHT_PUT(w, OHT_F_HUB_AEAD_SELFTEST, s->ex.aead_selftest);
        OHT_PUT(w, OHT_F_HUB_UPLINK_FRAMES, s->ex.uplink_frames);
        OHT_PUT(w, OHT_F_HUB_UPLINK_OK, s->ex.uplink_ok);
        OHT_PUT(w, OHT_F_HUB_UPLINK_BAD_TAG, s->ex.uplink_bad_tag);
        OHT_PUT(w, OHT_F_HUB_UPLINK_BAD_SLOT, s->ex.uplink_bad_slot);
        OHT_PUT(w, OHT_F_HUB_UPLINK_BAD_FRAME, s->ex.uplink_bad_frame);
        OHT_PUT(w, OHT_F_HUB_UPLINK_REPLAY, s->ex.uplink_replay);
        OHT_PUT(w, OHT_F_HUB_UPLINK_WINDOWS, s->ex.uplink_windows);
        OHT_PUT(w, OHT_F_HUB_UPLINK_SYNC, s->ex.uplink_sync);
        OHT_PUT(w, OHT_F_HUB_UPLINK_EVT_DROP, s->ex.uplink_evt_drop);
    }
    if (s->have_pair)
        OHT_PUT(w, OHT_F_HUB_PAIR_STATE, s->pair.state);
    if (s->have_dl) {
        OHT_PUT(w, OHT_F_HUB_DL_OPPORTUNITIES, s->dl.opportunities);
        OHT_PUT(w, OHT_F_HUB_DL_SENT, s->dl.sent);
        OHT_PUT(w, OHT_F_HUB_DL_TX_ERR, s->dl.tx_err);
        OHT_PUT(w, OHT_F_HUB_DL_CMD_SENT, s->dl.cmd_sent);
        OHT_PUT(w, OHT_F_HUB_DL_CMD_ACKED, s->dl.cmd_acked);
        OHT_PUT(w, OHT_F_HUB_DL_CMD_LOST, s->dl.cmd_lost);
        OHT_PUT(w, OHT_F_HUB_DL_NONCE_REFUSED, s->dl.nonce_refused);
    }
    if (s->have_lat) {
        OHT_PUT(w, OHT_F_HUB_EVT_RTT_LAST_US, s->lat.rtt_last_us);
        OHT_PUT(w, OHT_F_HUB_EVT_RTT_MAX_US, s->lat.rtt_max_us);
        OHT_PUT(w, OHT_F_HUB_EVT_LOST, s->lat.lost);
    }
}

static void put_rxdiag(oht_writer_t *w, const snap_t *s) {
    if (!s->have_rx)
        return;
    oht_rec_begin(w, OHT_OBJ_RXDIAG, 0);
    OHT_PUT(w, OHT_F_RXDIAG_SYNC_MATCH, s->rx.sync_match);
    OHT_PUT(w, OHT_F_RXDIAG_CRC_ERR, s->rx.crc_err);
    OHT_PUT(w, OHT_F_RXDIAG_FRAMES, s->rx.frames);
    OHT_PUT(w, OHT_F_RXDIAG_FLUSHES, s->rx.flushes);
    OHT_PUT(w, OHT_F_RXDIAG_RSSI_SAMPLES, s->rx.rssi_samples);
    OHT_PUT(w, OHT_F_RXDIAG_LAST_RSSI_DBM, s->rx.last_rssi);
    /* Zero is the firmware's "never sampled"; the server knows not to divide by it. */
    OHT_PUT(w, OHT_F_RXDIAG_UP_RSSI_PEAK_DBM, s->rx.up_rssi_peak);
    OHT_PUT(w, OHT_F_RXDIAG_UP_RSSI_FLOOR_DBM, s->rx.up_rssi_floor);
    OHT_PUT(w, OHT_F_RXDIAG_JOIN_RSSI_PEAK_DBM, s->rx.rssi_peak);
    OHT_PUT(w, OHT_F_RXDIAG_JOIN_RSSI_FLOOR_DBM, s->rx.rssi_floor);
}

static void put_link(oht_writer_t *w) {
    oht_rec_begin(w, OHT_OBJ_LINK, 0);
    OHT_PUT(w, OHT_F_LINK_CONNECTS, stats.connects);
    OHT_PUT(w, OHT_F_LINK_DISCONNECTS, stats.disconnects);
    OHT_PUT(w, OHT_F_LINK_LAST_DISC_REASON, stats.last_disc_reason);
    OHT_PUT(w, OHT_F_LINK_FRAMES_TX, stats.frames_tx);
    OHT_PUT(w, OHT_F_LINK_BYTES_TX, stats.bytes_tx);
    OHT_PUT(w, OHT_F_LINK_TX_FAIL, stats.tx_fail);
    OHT_PUT(w, OHT_F_LINK_CMDS_RX, stats.cmds_rx);
    OHT_PUT(w, OHT_F_LINK_CMDS_BAD, stats.cmds_bad);
    OHT_PUT(w, OHT_F_LINK_EVENTS_PUSHED, stats.events_pushed);
    OHT_PUT(w, OHT_F_LINK_EVENTS_DROPPED, stats.events_dropped);
    OHT_PUT(w, OHT_F_LINK_SNAPSHOT_US, stats.snapshot_us);
    /* Both, never one: asked and achieved parted company once already. */
    OHT_PUT(w, OHT_F_LINK_SNAPSHOT_ASK_MS, stats.snapshot_ms);
    OHT_PUT(w, OHT_F_LINK_SNAPSHOT_GAP_MS, stats.snapshot_gap_ms);
}

static void put_device(oht_writer_t *w, const ipc_device_report_t *d) {
    oht_rec_begin(w, OHT_OBJ_DEVICE, d->dev_id);
    OHT_PUT(w, OHT_F_DEVICE_SLOT, d->slot);
    OHT_PUT(w, OHT_F_DEVICE_LAST_SUPERFRAME, d->last_superframe);
    OHT_PUT(w, OHT_F_DEVICE_FRAMES_OK, d->frames_ok);
    OHT_PUT(w, OHT_F_DEVICE_FRAMES_BAD, d->frames_bad);
    OHT_PUT(w, OHT_F_DEVICE_DEV_UPTIME_S, d->uptime_s);
    OHT_PUT(w, OHT_F_DEVICE_REPORT_EVERY, d->report_every);
    OHT_PUT(w, OHT_F_DEVICE_REPORT_FLAGS, d->flags);
    OHT_PUT(w, OHT_F_DEVICE_ARRIVAL_US, d->arrival_us);
    /* Absent, never zero: zero would read as arrival exactly on the boundary. */
    if (d->arrival_sync_us != IPC_ARRIVAL_SYNC_NONE)
        OHT_PUT(w, OHT_F_DEVICE_ARRIVAL_SYNC_US, d->arrival_sync_us);
    /* Always sent: it is what tells an absent stamp from an unchanged one.
     * radio_devices_docs/open_hub/network/telemetry.md */
    OHT_PUT(w, OHT_F_DEVICE_SYNC_UNPAIRED, d->sync_unpaired);
    /* Always sent: a run of zero is the fact that the device is answering. */
    OHT_PUT(w, OHT_F_DEVICE_MISSED_RUN, d->missed_run);
    OHT_PUT(w, OHT_F_DEVICE_RSSI_UP_LATCH_DBM, d->rssi_up);
    OHT_PUT(w, OHT_F_DEVICE_RSSI_DOWN_DBM, d->rssi_down);
    OHT_PUT(w, OHT_F_DEVICE_CYC_MIN_MS, d->cyc_min);
    OHT_PUT(w, OHT_F_DEVICE_CYC_N, d->cyc_n);
    OHT_PUT(w, OHT_F_DEVICE_CYC_SUM_MS, d->cyc_sum);
    OHT_PUT(w, OHT_F_DEVICE_CMD_STATE, d->cmd_state);
    OHT_PUT(w, OHT_F_DEVICE_CMD_EVERY, d->cmd_every);
    OHT_PUT(w, OHT_F_DEVICE_ACK_ARG, d->ack_arg);
    /* Sent only when the flag says it was measured; a stale rail is not a reading. */
    if ((d->flags & RADIO_REPORT_FLAG_SUPPLY_STALE) == 0u)
        OHT_PUT(w, OHT_F_DEVICE_SUPPLY_MV, d->supply_mv);
    if ((d->flags & RADIO_REPORT_FLAG_TEMP_STALE) == 0u)
        OHT_PUT(w, OHT_F_DEVICE_TEMP_C_X10, d->temp_c_x10);
}

/**
 * @brief Adds the sync-match level and the AGC gain for one device's newest frame.
 *
 * Two RSSI fields exist because two instruments do. `rssi_up_latch_dbm` is
 * RegRssiValue with nothing triggering it (ROADMAP item 14); this one is the
 * level at the SyncAddressMatch edge, and only entries the ring marks
 * `in_frame` carry it. radio_devices_docs/open_hub/network/telemetry.md
 */
/* The ring records the opportunity, the device owns three of them. ROADMAP item 45
 * radio_devices_docs/open_hub/network/telemetry.md */
static int row_is_device(uint8_t opportunity, uint8_t slot) {
    /* Unplaceable first: 0xFF % RADIO_SLOT_STRIDE is 60, a real device. */
    if (opportunity == 0xFFu)
        return 0;
    return RADIO_SLOT_TO_DEVICE(opportunity) == slot;
}

static void put_device_air(oht_writer_t *w, const snap_t *s, uint8_t slot) {
    uint8_t i;

    if (!s->have_afc)
        return;
    for (i = 0; i < s->afc.n && i < IPC_AFC_RING; i++) {
        if (!row_is_device(s->afc.slot[i], slot))
            continue;
        if ((s->afc.in_frame & (1u << i)) == 0u)
            continue;
        OHT_PUT(w, OHT_F_DEVICE_RSSI_UP_SYNC_DBM, s->afc.rssi[i]);
        OHT_PUT(w, OHT_F_DEVICE_LNA_GAIN, s->afc.gain[i]);
        OHT_PUT(w, OHT_F_DEVICE_AFC_HZ, s->afc.afc_hz[i]);
        return;
    }
}

/* The ring is newest first, so `total` minus the index names the sample. */
static void put_frames(oht_writer_t *w, const snap_t *s, uint8_t slot,
                       uint32_t dev_id) {
    uint8_t i;

    if (!s->have_afc)
        return;
    for (i = 0; i < s->afc.n && i < IPC_AFC_RING; i++) {
        if (!row_is_device(s->afc.slot[i], slot))
            continue;
        oht_rec_begin(w, OHT_OBJ_FRAME, dev_id);
        OHT_PUT(w, OHT_F_FRAME_SEQ, s->afc.total - i);
        OHT_PUT(w, OHT_F_FRAME_GRID, s->afc.grid[i]);
        OHT_PUT(w, OHT_F_FRAME_SLOT, s->afc.slot[i]);
        OHT_PUT(w, OHT_F_FRAME_RSSI_DBM, s->afc.rssi[i]);
        OHT_PUT(w, OHT_F_FRAME_LNA_GAIN, s->afc.gain[i]);
        OHT_PUT(w, OHT_F_FRAME_AFC_HZ, s->afc.afc_hz[i]);
        OHT_PUT(w, OHT_F_FRAME_CRC_OK, (s->afc.crc_ok & (1u << i)) != 0u);
        OHT_PUT(w, OHT_F_FRAME_IN_FRAME, (s->afc.in_frame & (1u << i)) != 0u);
    }
}

static void gather(snap_t *s) {
    memset(s, 0, sizeof(*s));
    s->have_timing = fetch(IPC_REQ_GET_TIMING, 0, &s->timing, sizeof(s->timing));
    s->have_ex     = fetch(IPC_REQ_GET_EXCHANGE, 0, &s->ex, sizeof(s->ex));
    s->have_rx     = fetch(IPC_REQ_GET_RXDIAG, 0, &s->rx, sizeof(s->rx));
    s->have_dl     = fetch(IPC_REQ_GET_DOWNLINK, 0, &s->dl, sizeof(s->dl));
    s->have_lat    = fetch(IPC_REQ_GET_EVT_LAT, 0, &s->lat, sizeof(s->lat));
    s->have_pair   = fetch(IPC_REQ_GET_PAIR_STATE, 0, &s->pair, sizeof(s->pair));
    s->have_afc    = fetch(IPC_REQ_GET_AFC_RAW, 0, &s->afc, sizeof(s->afc));
}

/* One writer per frame; a body the buffer refused is counted, never truncated. */
static int flush_body(oht_writer_t *w, uint8_t type, uint16_t flags) {
    if (OHT_FAILED(w)) {
        stats.snapshot_trunc++;
        return 0;
    }
    if (w->len == 0u)
        return 0;
    return send_frame(type, w->buf, w->len, 0, flags);
}

static int send_snapshot(void) {
    static uint32_t last_snapshot_tk;
    uint32_t t0 = osKernelGetTickCount();
    oht_writer_t w;
    snap_t s;
    uint8_t i, in_frame = 0;

    if (last_snapshot_tk != 0u)
        stats.snapshot_gap_ms = t0 - last_snapshot_tk;
    last_snapshot_tk = t0;
    gather(&s);

    oht_writer_init(&w, tx_buf, sizeof(tx_buf));
    put_hub(&w, &s);
    put_rxdiag(&w, &s);
    put_link(&w);
    if (flush_body(&w, OHT_MSG_SNAPSHOT, 0) != 0)
        return -1;

    if (!s.have_ex)
        goto done;

    oht_writer_init(&w, tx_buf, sizeof(tx_buf));
    for (i = 0; i < s.ex.devices; i++) {
        ipc_device_report_t d;

        if (!fetch(IPC_REQ_GET_DEVICE_INFO, i, &d, sizeof(d)))
            break;
        put_device(&w, &d);
        put_device_air(&w, &s, d.slot);
        put_frames(&w, &s, d.slot, d.dev_id);
        in_frame++;

        if (in_frame >= DEVICES_PER_FRAME) {
            if (flush_body(&w, OHT_MSG_SNAPSHOT, 0) != 0)
                return -1;
            oht_writer_init(&w, tx_buf, sizeof(tx_buf));
            in_frame = 0;
        }
    }
    if (in_frame != 0u && flush_body(&w, OHT_MSG_SNAPSHOT, 0) != 0)
        return -1;

done:
    stats.snapshots++;
    /* Ticks, in the unit the field names; the IPC round trips dominate it. */
    stats.snapshot_us = (osKernelGetTickCount() - t0) * 1000u;
    return 0;
}

static int send_uplink_event(const ipc_device_report_t *d) {
    oht_writer_t w;

    oht_writer_init(&w, tx_buf, sizeof(tx_buf));
    put_device(&w, d);
    if (flush_body(&w, OHT_MSG_EVENT, OHT_EVT_UPLINK) != 0)
        return -1;
    stats.events_pushed++;
    return 0;
}

/* ---- commands ---------------------------------------------------------- */

static uint8_t arg_u32(const uint8_t *body, uint16_t len, uint16_t id,
                       uint32_t *out) {
    oht_field_t f;
    int64_t v = 0;

    if (oht_find(body, len, id, &f) != 1 || oht_field_int(&f, &v) != 0)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static uint8_t arg_u8(const uint8_t *body, uint16_t len, uint16_t id,
                      uint8_t *out) {
    uint32_t v = 0;

    if (!arg_u32(body, len, id, &v) || v > 255u)
        return 0;
    *out = (uint8_t)v;
    return 1;
}

/* Repeats default to the console's four, so both paths ride the same downlinks. */
static uint8_t arg_repeats(const uint8_t *body, uint16_t len) {
    uint8_t n = 4;

    (void)arg_u8(body, len, 0x8005u, &n);
    return (n == 0u) ? 1u : n;
}

static uint8_t do_device_cmd(uint32_t target, uint8_t radio_cmd, uint8_t every,
                             const uint8_t *body, uint16_t len, uint8_t *detail) {
    ipc_device_cmd_t c;
    int rc;

    memset(&c, 0, sizeof(c));
    c.dev_id       = target;
    c.cmd          = radio_cmd;
    c.report_every = every;
    c.repeats      = arg_repeats(body, len);

    rc = hub_ipc_call(IPC_REQ_SET_DEVICE_PARAM, 0, &c, (uint8_t)sizeof(c), &reply);
    *detail = (uint8_t)((rc < 0) ? 0xFFu : rc);
    if (rc == IPC_ST_BAD_ARG)
        return OHT_RES_NO_SUCH_DEVICE;
    if (rc != IPC_ST_OK)
        return OHT_RES_RADIO_ERR;
    /* Queued, never ok: nothing on the wire has acknowledged it yet. */
    return OHT_RES_QUEUED;
}

static uint8_t handle_cmd(const oht_cmd_hdr_t *h, const uint8_t *body,
                          uint16_t len, uint8_t *detail) {
    uint8_t u8v = 0;
    uint32_t u32v = 0;

    *detail = 0;
    switch (h->cmd_id) {
    case OHT_CMD_PING:
        return OHT_RES_OK;

    case OHT_CMD_SNAPSHOT_NOW:
        snapshot_now = 1;
        return OHT_RES_OK;

    case OHT_CMD_SET_REPORT_RATE: {
        int rc;

        if (!arg_u8(body, len, 0x8001u, &u8v) || u8v == 0u)
            return OHT_RES_BAD_ARGS;
        rc = hub_ipc_call(IPC_REQ_SET_REPORT_RATE, u8v, NULL, 0, &reply);
        *detail = (uint8_t)((rc < 0) ? 0xFFu : rc);
        if (rc != IPC_ST_OK)
            return OHT_RES_RADIO_ERR;
        pairing_set_report_every(u8v);
        return OHT_RES_OK;
    }

    case OHT_CMD_PAIR_WINDOW: {
        uint32_t window = RADIO_PAIR_WINDOW_MS;

        if (!arg_u32(body, len, 0x8000u, &u32v) || u32v == 0u)
            return OHT_RES_BAD_ARGS;
        (void)arg_u32(body, len, 0x8002u, &window);
        {
            const cfg_device_t *have = cfg_find(u32v);

            if (have == NULL || have->state == CFG_DEV_FREE)
                return OHT_RES_NO_SUCH_DEVICE;
        }
        pairing_arm_init(u32v, window);
        return OHT_RES_OK;
    }

    case OHT_CMD_DEVICE_ADD: {
        uint8_t slot = 0;
        int rc;

        /* An id and nothing else; the device's key arrives in PAIR_REQ. ADR-0024 */
        if (!arg_u32(body, len, 0x8000u, &u32v) || u32v == 0u)
            return OHT_RES_BAD_ARGS;
        if (cfg_enrol(u32v, &slot) != CFGF_OK)
            return OHT_RES_BUSY;
        rc = hub_ipc_call(IPC_REQ_ADD_DEVICE, 0, &u32v, sizeof(u32v), &reply);
        *detail = slot;
        if (rc != IPC_ST_OK)
            return OHT_RES_RADIO_ERR;
        pairing_arm_init(u32v, RADIO_PAIR_WINDOW_MS);
        return OHT_RES_OK;
    }

    case OHT_CMD_DEVICE_REMOVE: {
        int rc;

        if (!arg_u32(body, len, 0x8000u, &u32v) || u32v == 0u)
            return OHT_RES_BAD_ARGS;
        if (cfg_forget(u32v) != CFGF_OK)
            return OHT_RES_NO_SUCH_DEVICE;
        /* Half a removal; the radio has its own entry.
         * radio_devices_docs/open_hub/arch/ipc.md */
        rc = hub_ipc_call(IPC_REQ_REMOVE_DEVICE, 0, &u32v, sizeof(u32v), &reply);
        *detail = (uint8_t)((rc < 0) ? 0xFFu : rc);
        /* Reported, not rolled back: the store write happened. */
        return (rc == IPC_ST_OK) ? OHT_RES_OK : OHT_RES_RADIO_ERR;
    }

    case OHT_CMD_SET_LNA: {
        int rc;

        if (!arg_u8(body, len, 0x8004u, &u8v))
            return OHT_RES_BAD_ARGS;
        rc = hub_ipc_call(IPC_REQ_SET_LNA, u8v, NULL, 0, &reply);
        *detail = (uint8_t)((rc < 0) ? 0xFFu : rc);
        return (rc == IPC_ST_OK) ? OHT_RES_OK : OHT_RES_RADIO_ERR;
    }

    case OHT_CMD_DEV_SET_RATE:
        if (!arg_u8(body, len, 0x8001u, &u8v) || u8v == 0u)
            return OHT_RES_BAD_ARGS;
        return do_device_cmd(h->target, RADIO_CMD_SET_RATE, u8v, body, len, detail);

    case OHT_CMD_DEV_REJOIN:
        return do_device_cmd(h->target, RADIO_CMD_REJOIN, 0, body, len, detail);

    case OHT_CMD_DEV_NOP:
        return do_device_cmd(h->target, RADIO_CMD_NOP, 0, body, len, detail);

    case OHT_CMD_DEV_APP:
        /* No wire for app[6] yet; it binds the WL55. ROADMAP item 3,
         * radio_devices_docs/radio/tdma.md */
        return OHT_RES_NOT_IMPLEMENTED;

    default:
        return OHT_RES_UNKNOWN_CMD;
    }
}

static int serve_cmd(const oht_hdr_t *h, const uint8_t *payload) {
    oht_cmd_ack_t ack;
    oht_cmd_hdr_t ch;
    uint8_t detail = 0;

    stats.cmds_rx++;
    memset(&ack, 0, sizeof(ack));
    ack.cmd_seq = h->seq;

    if (h->len < sizeof(ch)) {
        stats.cmds_bad++;
        ack.result = OHT_RES_BAD_ARGS;
    } else {
        memcpy(&ch, payload, sizeof(ch));
        ack.result = handle_cmd(&ch, payload + sizeof(ch),
                                (uint16_t)(h->len - sizeof(ch)), &detail);
        ack.detail = detail;
        if (ack.result == OHT_RES_BAD_ARGS || ack.result == OHT_RES_UNKNOWN_CMD)
            stats.cmds_bad++;
    }
    return send_frame(OHT_MSG_CMD_ACK, (const uint8_t *)&ack, sizeof(ack), 0, 0);
}

/* ---- the session ------------------------------------------------------- */

static int say_hello(void) {
    oht_hello_t hello;
    oht_hello_ack_t ack;
    oht_hdr_t h;

    memset(&hello, 0, sizeof(hello));
    hello.magic     = OHT_MAGIC;
    hello.proto_ver = OHT_VERSION;
    hello.hub_id    = PAIRING_HUB_ID;
    hello.boot_id   = boot_id;
    hello.uptime_ms = osKernelGetTickCount();
    snprintf(hello.fw, sizeof(hello.fw), "openhub-cm7");
    memcpy(hello.build, BUILD_ID, sizeof(BUILD_ID));
    memcpy(hello.schema_digest, OHT_SCHEMA_DIGEST, sizeof(hello.schema_digest) - 1u);
    memcpy(hello.pair_digest, PAIR_VECTORS_DIGEST, sizeof(hello.pair_digest) - 1u);
    memcpy(hello.hop_digest, HOP_VECTORS_DIGEST, sizeof(hello.hop_digest) - 1u);
    if (cfg.token_len != 0u)
        memcpy(hello.token, cfg.token, cfg.token_len);

    if (send_frame(OHT_MSG_HELLO, (const uint8_t *)&hello, sizeof(hello), 0, 0) != 0)
        return -1;
    if (recv_exact((uint8_t *)&h, sizeof(h), CONNECT_TIMEOUT_MS) != 0)
        return -1;
    if (h.ver != OHT_VERSION || h.type != OHT_MSG_HELLO_ACK ||
        h.len != sizeof(ack))
        return -1;
    if (recv_exact((uint8_t *)&ack, sizeof(ack), CONNECT_TIMEOUT_MS) != 0)
        return -1;

    stats.hello_reason = ack.reason;
    if (!ack.accepted)
        return -1;
    /* The server names a cadence; the hub clamps it rather than obeying a zero. */
    stats.snapshot_ms = (ack.snapshot_ms < SNAPSHOT_MS_MIN) ? SNAPSHOT_MS_MIN
                                                            : ack.snapshot_ms;
    return 0;
}

static int try_connect(void) {
    struct sockaddr_in addr;
    struct timeval tv;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = lwip_htons(cfg.port);
    if (ip4addr_aton(cfg.ip, (ip4_addr_t *)&addr.sin_addr) == 0)
        return -1;

    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    /* No SO_RCVTIMEO here: it is compiled out. wait_readable() is the poll. */
    tv.tv_sec  = CONNECT_TIMEOUT_MS / 1000u;
    tv.tv_usec = 0;
    (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        lwip_close(sock);
        sock = -1;
        return -1;
    }
    return 0;
}

/* Returns when the link is gone; the reason is already latched in stats. */
static void run_session(void) {
    uint32_t next_snapshot = osKernelGetTickCount();
    uint32_t next_ping = next_snapshot + KEEPALIVE_MS;

    stats.connects++;
    stats.connected = 1;
    session_start_ms = osKernelGetTickCount();
    last_rx_ms = session_start_ms;

    for (;;) {
        oht_hdr_t h;
        ipc_device_report_t d;
        uint32_t now;
        int r;

        if (!stats.enabled) {
            disconnect(OHT_DISC_REASON_OPERATOR);
            return;
        }

        r = recv_exact((uint8_t *)&h, sizeof(h), POLL_MS);
        if (r < 0) {
            disconnect(OHT_DISC_REASON_PEER_CLOSED);
            return;
        }
        if (r == 0) {
            last_rx_ms = osKernelGetTickCount();
            if (h.ver != OHT_VERSION || h.len > sizeof(rx_buf)) {
                disconnect(OHT_DISC_REASON_BAD_FRAME);
                return;
            }
            /* Past the header a timeout is a torn frame, not an idle socket. */
            if (h.len != 0u && recv_exact(rx_buf, h.len, POLL_MS) != 0) {
                disconnect(OHT_DISC_REASON_BAD_FRAME);
                return;
            }
            if (h.type == OHT_MSG_CMD) {
                if (serve_cmd(&h, rx_buf) != 0) {
                    disconnect(OHT_DISC_REASON_SEND_FAILED);
                    return;
                }
            } else if (h.type == OHT_MSG_PING) {
                if (send_frame(OHT_MSG_PONG, NULL, 0, h.seq, 0) != 0) {
                    disconnect(OHT_DISC_REASON_SEND_FAILED);
                    return;
                }
            }
        }

        now = osKernelGetTickCount();
        stats.up_ms = now - session_start_ms;

        /* Asked for, not waited for: a silent peer would trip the timeout. */
        if ((int32_t)(now - next_ping) >= 0) {
            next_ping = now + KEEPALIVE_MS;
            if (send_frame(OHT_MSG_PING, NULL, 0, 0, 0) != 0) {
                disconnect(OHT_DISC_REASON_SEND_FAILED);
                return;
            }
        }

        if ((uint32_t)(now - last_rx_ms) > IDLE_TIMEOUT_MS) {
            disconnect(OHT_DISC_REASON_KEEPALIVE_LOST);
            return;
        }

        /* Arrivals go first: only they have a deadline. ROADMAP item 2 */
        while (uplink_q != NULL &&
               osMessageQueueGet(uplink_q, &d, NULL, 0) == osOK) {
            if (send_uplink_event(&d) != 0) {
                disconnect(OHT_DISC_REASON_SEND_FAILED);
                return;
            }
        }

        if (snapshot_now || (int32_t)(now - next_snapshot) >= 0) {
            snapshot_now = 0;
            next_snapshot = now + stats.snapshot_ms;
            if (send_snapshot() != 0) {
                disconnect(OHT_DISC_REASON_SEND_FAILED);
                return;
            }
        }
    }
}

/* ---- the public surface ------------------------------------------------ */

int telemetry_configure(const char *ip, uint16_t port, const char *token) {
    ip4_addr_t parsed;

    if (ip == NULL || ip4addr_aton(ip, &parsed) == 0)
        return -1;
    snprintf(cfg.ip, sizeof(cfg.ip), "%s", ip);
    cfg.port = port;
    memset(cfg.token, 0, sizeof(cfg.token));
    cfg.token_len = 0;
    if (token != NULL) {
        size_t n = strlen(token);

        if (n > sizeof(cfg.token))
            n = sizeof(cfg.token);
        memcpy(cfg.token, token, n);
        cfg.token_len = (uint8_t)n;
    }
    return 0;
}

void telemetry_enable(uint8_t on) {
    stats.enabled = on ? 1u : 0u;
    if (!on && sock >= 0)
        lwip_shutdown(sock, SHUT_RDWR);
}

void telemetry_request_snapshot(void) {
    snapshot_now = 1;
}

const telemetry_stats_t *telemetry_get_stats(const char **ip, uint16_t *port) {
    if (ip != NULL)
        *ip = cfg.ip;
    if (port != NULL)
        *port = cfg.port;
    return &stats;
}

void telemetry_notify_uplink(const ipc_device_report_t *r) {
    if (uplink_q == NULL || r == NULL)
        return;
    if (osMessageQueuePut(uplink_q, r, 0, 0) != osOK)
        stats.events_dropped++;
}

void TelemetryTask(void *argument) {
    (void)argument;

    uplink_q = osMessageQueueNew(UPLINK_QUEUE_LEN, sizeof(ipc_device_report_t), NULL);
    stats.snapshot_ms = SNAPSHOT_MS_DEF;
    /* Redrawn per boot: a reconnect and a reset must not look alike. */
    if (rng_word(&boot_id) != RNG_OK)
        boot_id = osKernelGetTickCount();

    /* DHCP needs about twenty seconds; connecting sooner only burns retries.
     * radio_devices_docs/open_hub/network/ethernet.md */
    osDelay(5000);

    for (;;) {
        if (!stats.enabled || cfg.ip[0] == '\0') {
            osDelay(RECONNECT_MS);
            continue;
        }
        if (try_connect() != 0) {
            stats.connect_fail++;
            stats.last_disc_reason = OHT_DISC_REASON_CONNECT_FAILED;
            osDelay(RECONNECT_MS);
            continue;
        }
        if (say_hello() != 0) {
            disconnect(OHT_DISC_REASON_HELLO_REFUSED);
            osDelay(RECONNECT_MS);
            continue;
        }
        run_session();
        osDelay(RECONNECT_MS);
    }
}
