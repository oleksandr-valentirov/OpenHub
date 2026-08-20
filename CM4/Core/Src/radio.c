#include <string.h>

#include "radio.h"
#include "rfm69.h"
#include "rfm69_registers.h"
#include "timebase.h"
#include "hsem_table.h"
#include "shared_memory.h"
#include "ipc.h"
#include "radio_protocol.h"
#include "radio_slots.h"
#include "radio_phy.h"
#include "hop.h"
#include "hop_v1.h"
#include "pair_v2.h"
#include "calib.h"
#include "kvstore.h"
#include "aead.h"
#include "main.h"

/* The PHY is radio_phy.h now. It used to be defined here, where the device side
 * could not see it and had to write its own copy of every number - which is the
 * arrangement that let PAIR_FRAME_LEN be 45 on one side and 49 on the other with
 * an assert passing on each. 25 kbps GFSK, 25 kHz deviation, 100 kHz RxBw;
 * hopping stays inside 865-868 MHz, the sub-band that allows 1% duty cycle. */

/* RADIO_PROTO_VERSION now comes from radio_protocol.h. It was defined here,
 * where the device side could not see it, and the device had chosen 1 for the
 * pairing frames while the beacons went out as 2. */
/* A joining device parks on the fixed channel, so the window only has to be
 * short enough to bound the extra airtime and long enough for a human. */
#define PAIRING_WINDOW_MS       RADIO_PAIR_WINDOW_MS
#define JOIN_BEACON_EVERY       2u

#define BROADCAST_ADDR          255

#define MODE_TIMEOUT_US         10000u
/* A measurement is 2^(smoothing+1) bit periods - microseconds at 25 kbps. This
 * bounds an SPI fault, not the part; it must stay far below the slot guard. */
#define RSSI_TIMEOUT_US         500u
#define TX_TIMEOUT_US           200000u

/* Slots are assigned from 0 upward and the store holds at most KS_MAX_DEVICES,
 * so a slot at or above this can never be handed out. Indexing the table by
 * slot is what lets an uplink frame carry a slot instead of a device id. */
#define RADIO_MAX_DEVICES  64u

/* One exchange at a time, and these bound how long a half-finished one may
 * hold the machine. CM7 needs ~330 ms of curve work; a device needs ~205 ms
 * plus a round trip. Both sit well inside the quiesce they run in. */
#define EX_CM7_TIMEOUT_US   2000000u
#define EX_DEV_TIMEOUT_US   3000000u

typedef struct dev_entry {
    uint8_t  used;
    uint8_t  slot;
    uint8_t  report_every;
    uint8_t  flags;             /* RADIO_REPORT_FLAG_* from the last report */
    uint32_t dev_id;
    uint32_t key_gen;
    uint8_t  session_key[AEAD_KEY_BYTES];
    uint32_t last_superframe;
    /* Highest superframe accepted from this device, and the only thing that
     * makes an uplink unreplayable. Scoped to key_gen: a re-pair changes the
     * session key, so a frame from an older generation cannot verify anyway
     * and the floor starts again at zero. */
    uint32_t rx_floor;
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t frames_replay;
    uint32_t uptime_s;
    uint16_t supply_mv;
    int8_t   rssi_up;           /* measured here, on the device's last frame */
    int8_t   rssi_down;         /* as the device heard the hub's last beacon */
} dev_entry_t;

static void RFM_send_broadcast(uint8_t flags, uint8_t resume_in);
static void RFM_send_join_beacon(void);
static uint8_t RFM_open_pairing(uint32_t dev_id);
static void RFM_serve_request(const ipc_msg_t *req);
static int superframe_due(void);
static void on_superframe(void);
static void join_region_service(void);
static uint8_t begin_quiesce(uint8_t superframes);
static void handle_join_frame(void);
static int  frame_selftest(void);
static void ex_reset(void);
static void exchange_service(void);
static void uplink_service(void);
static void downlink_service(void);
static int  install_device(const ipc_device_keys_t *k);


static rfm69_dev_t radio;
static uint8_t tx_buffer[RFM69_FIFO_SIZE];
static uint32_t hub_id = 0x33442211u;
static uint32_t frame_counter = 0;
static uint32_t superframe_start_tk = 0;
static uint32_t superframe_tk = 0;   /* SUPERFRAME_US in real ticks */
static uint8_t  grid_started = 0;
static uint32_t late_last_us = 0;
static uint32_t late_max_us = 0;
static uint32_t late_min_us = 0xFFFFFFFFu;
static uint32_t late_over = 0;   /* beacons past RADIO_BEACON_LATE_LIMIT_US */
static uint32_t pairing_deadline_us = 0;
static uint8_t  pairing_open = 0;
static uint32_t pairing_dev_id = 0;

/* Offsets into the superframe, in real ticks. Recomputed at each boundary from
 * the same calibration the period uses, so the grid does not drift apart
 * internally when the measured scale moves. */
static uint32_t join_offset_tk = 0;

static radio_pair_state_t pair_state = RADIO_PAIR_IDLE;
static uint8_t  quiesce_len = 0;         /* superframes announced */
static uint32_t quiesce_resume_at = 0;   /* the counter value promised on air */
static uint8_t  quiesce_pending = 0;     /* announce at the next boundary */
/* Starts one full gap in the past so the first quiesce is not refused. Signed
 * differences everywhere below, so this survives the counter wrap. */
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
/* The beacon path can fail at four points and every one of them used to return
 * silently. data_beacons counts attempts, so it stays equal to the superframes
 * elapsed and the accounting invariant holds - but a radio failing every
 * transmit would otherwise report identically to a working one. */
static uint32_t beacon_err = 0;
static uint32_t quiesce_lost = 0;        /* a valid PAIR_REQ that won no clear air */

/* Join-region sub-state, so a 100 ms receive window does not block the loop and
 * delay the next superframe boundary - the beacon's own jitter is the thing
 * this whole grid is measured against. */
static uint8_t  join_phase = 0;
static uint8_t  join_beacon_pending = 0;
static uint32_t join_rx_deadline = 0;
static uint32_t join_served_frame = 0xFFFFFFFFu;
static uint8_t  rx_buffer[RFM69_FIFO_SIZE];

/* Indexed by slot, so an uplink frame's slot byte is the whole lookup and a
 * frame naming an unassigned slot is refused before any crypto runs. */
static dev_entry_t devices[RADIO_MAX_DEVICES];
static uint8_t  device_count;
static uint8_t  net_hop_key_set;
static uint8_t  report_every_grant = RADIO_REPORT_EVERY_DEFAULT;
static int      aead_selftest_rc = 1;   /* until it has actually run */

static radio_exchange_state_t ex_state = RADIO_EX_IDLE;
static uint16_t ex_seq;
static uint32_t ex_dev_id;
static uint32_t ex_deadline;
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
/* The downlink is one frame per opportunity, round-robined, so this is where
 * the rotation is. Held across superframes: restarting at slot 0 each time
 * would serve the first device every opportunity and the rest never. */
static uint8_t  dl_next_slot;
static uint32_t dl_sent, dl_seal_err, dl_tx_err, dl_no_device, dl_served;
static uint32_t dl_prf_err, dl_last_hz, dl_last_sf;

/* pair_v3's invitation, built and MACed by CM7. Opaque here: CM4 keys the bytes
 * at the superframe it was told and forms no opinion about them. */
static uint8_t  pi_frame[RADIO_PAIR_INIT_MAX];
static uint8_t  pi_len;
static uint32_t pi_superframe;      /* 0 when nothing is queued */
static uint32_t pi_given, pi_sent, pi_missed, pi_tx_err, pi_replaced;
static uint32_t pi_frf;      /* RegFrf read back after the transmit */
static uint8_t  pi_paylen;
static uint32_t pi_last_sent_sf;
static uint32_t dl_opportunities;
static int16_t  up_rssi_peak_x2 = -32768, up_rssi_floor_x2 = 32767;
static uint32_t rx_crc_err;
static uint32_t rx_sync_match, rx_frames;
static uint8_t  sync_was_set;
static uint8_t  rx_last_len, rx_last_type;
static int8_t   rx_last_rssi;
/* What the refused frame actually carried. Naming the field that mismatched
 * still leaves two implementations each certain they wrote the same number. */
static uint16_t reqs_drop_net;
static uint32_t reqs_drop_hub, reqs_drop_dev;
static uint8_t  reqs_drop_head[16];
static uint8_t  reqs_drop_key[8];
static uint32_t rx_last_superframe;
/* dbm_x2 from the driver is negated raw, so a STRONGER signal is a LARGER
 * (less negative) number. Written the other way round first, which made the
 * peak read weaker than the floor - nonsense on its face, and the only reason
 * it was caught in the first reading rather than in a conclusion. */
static int16_t  rx_rssi_peak_x2 = -32768;   /* strongest sample seen */
static int16_t  rx_rssi_floor_x2 = 32767;   /* weakest */
/* Peak and floor mean nothing without it: both read 0 when the window never
 * opened and when every measurement failed, and those are different faults. */
static uint32_t rx_rssi_samples;
static uint8_t  beacon_err_last, reqs_drop_last;

/* The uplink region is one long receive on the channel the beacon just went
 * out on. The slot grid exists to keep devices from colliding with each other;
 * the hub has one receiver and no reason to retune 96 times. */
static uint8_t  uplink_rx_open;

extern CRYP_HandleTypeDef hcryp;
static hop_ctx_t hop;

/* The network hop key. Zero-valued until CM7 installs the real one - except
 * that "zero" here is not zeros: see hop_key_placeholder(). */
static uint8_t  net_hop_key[RADIO_HOP_KEY_BYTES];

/* One AES-128 block through CRYP, with everything it needs stated.
 *
 * Inheriting is how two correct functions produce one wrong answer, and this
 * function has proved it twice. It shares the accelerator with the frame
 * cipher, which sets a per-device key and a byte data width on every frame it
 * touches. Inheriting the key would hop according to whichever device last
 * transmitted; inheriting DataWidthUnit truncated the block to four bytes and
 * produced a perfectly valid permutation that no device could follow.
 *
 * So: algorithm, key, key size, data width and header size, every time. */
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
    /* 8-bit, not the 32-bit the .ioc configures: with a 32-bit datatype the
     * accelerator takes the buffer word-wise, so every group of four bytes is
     * reversed on this little-endian core. Measured, not reasoned. */
    cfg.DataType      = CRYP_DATATYPE_8B;
    cfg.DataWidthUnit = CRYP_DATAWIDTHUNIT_BYTE;
    cfg.HeaderSize    = 0;
    if (HAL_CRYP_SetConfig(&hcryp, &cfg) != HAL_OK)
        return -1;
    /* 16 because the width unit above says bytes. A count in the other unit is
     * not an error the HAL reports - one direction truncates and the other
     * over-reads, and only truncation changes the answer. */
    if (HAL_CRYP_Encrypt(&hcryp, (uint32_t *)(void *)in, 16,
                         (uint32_t *)(void *)out, 50) != HAL_OK)
        return -1;
    return 0;
}

/* Runs once per full hop cycle - about a minute - so the cost is irrelevant,
 * but the accelerator is here and idle.
 *
 * A PRF failure must propagate: Fisher-Yates over an uninitialised buffer still
 * produces a perfectly valid permutation, so a silent failure looks exactly
 * like a working hop sequence that no device can follow. */
static int hop_prf_aes(void *ctx, const uint8_t in[16], uint8_t out[16]) {
    (void)ctx;
    return aes_ecb_block(net_hop_key, in, out);
}

/* The hop key before any device has paired. It is NOT secret and is not a
 * stand-in for the network key - it exists only so that two hubs in this state
 * do not follow the *same* sequence.
 *
 * Zeros would do that: the all-zero key is not random, so two unpaired hubs
 * sharing a bench would land on the identical channel every superframe and
 * interfere deterministically, which is the one collision nobody would think to
 * diagnose. Derived from hub_id instead, which is already unique per hub and
 * already public. Raised by the device side.
 *
 * Beaconing on a per-hub sequence beats not beaconing at all: the SDR bench
 * this project is verified with has nothing to capture from a silent hub. */
static void hop_key_placeholder(void) {
    for (unsigned i = 0; i < RADIO_HOP_KEY_BYTES; i++)
        net_hop_key[i] = (uint8_t)((hub_id >> (8u * (i & 3u))) ^ (0x5Au + i));
}

/* The PRF against a host-computed AES block, at boot, after the frame cipher
 * has had CRYP - which is the adversarial ordering, not the convenient one.
 *
 * This existed only as a console command, so the defect it catches needed a
 * human to type something. No check on the *sequence* can see it: a truncated
 * block still shuffles to a uniform permutation with correct occupancy and
 * spread. The key and the input are both non-zero so a byte-order or width
 * error cannot hide in a block of zeroes. */
static int hop_prf_selftest(void) {
    uint8_t out[16];

    /* FIPS-197 C.1 first, and *this* is the check that anchors it.
     *
     * Both sides' vector generators call the same OpenSSL, and neither side
     * transcribed these bytes from the standard - one produced them, the other
     * copied them from a message. So the host-side assert is one implementation
     * agreeing with itself. CRYP is not OpenSSL, so running the block here is a
     * software-against-hardware cross-check and is where the independence
     * actually comes from. Do not delete this on the grounds that the vector
     * file already checks it. */
    if (aes_ecb_block(HV_FIPS_KEY, HV_FIPS_IN, out) != 0)
        return -1;
    if (memcmp(out, HV_FIPS_OUT, sizeof(out)) != 0)
        return -2;

    /* Then the real hop key against the cycle-1 counter block. Cycle 1 and not
     * cycle 0: cycle 0's block is all zeroes and identical under either endian
     * convention, so a check built on it passes for the hub's first 56 seconds
     * and fails for ever afterwards. */
    if (aes_ecb_block(HV_HOP_KEY, HV_PRF_IN, out) != 0)
        return -3;
    return (memcmp(out, HV_PRF_OUT, sizeof(out)) == 0) ? 0 : -4;
}

static uint32_t slot_hz(uint32_t slot) {
    return RADIO_SLOT_HZ(slot);
}

/* hop.c yields 0..RADIO_HOP_COUNT-1; the reserved join slot is skipped so the
 * hopping set and the join channel are disjoint by construction. */
static uint32_t hop_slot_to_grid(uint8_t hop_index) {
    return RADIO_HOP_TO_GRID(hop_index);
}

/* --- platform glue: everything the driver needs from this board --- */

static int spi_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    return rfm_spi_transfer(tx, rx, len);
}

static void spi_select(void *ctx, int asserted) {
    (void)ctx;
    HAL_GPIO_WritePin(RFM_CS_GPIO_Port, RFM_CS_Pin, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void radio_reset(void *ctx, int asserted) {
    (void)ctx;
    HAL_GPIO_WritePin(RFM_RESET_GPIO_Port, RFM_RESET_Pin, asserted ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void radio_delay_us(void *ctx, uint32_t us) {
    (void)ctx;
    delay_us_poll(us);
}

static uint32_t radio_micros(void *ctx) {
    (void)ctx;
    return rfm_micros();
}

static const rfm69_io_t radio_io = {
    .transfer = spi_transfer,
    .select   = spi_select,
    .reset    = radio_reset,
    .delay_us = radio_delay_us,
    .micros   = radio_micros,
    .ctx      = NULL
};

uint8_t RFM_Init(uint8_t network_id, uint8_t node_id) {
    static const uint8_t sync_val[] = {'h', 'e', 'l', 'l'};
    (void)network_id;

    if (rfm69_init(&radio, &radio_io) != RFM69_OK)
        return 1;
    if (rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US) != RFM69_OK)
        return 1;

    if (rfm69_set_bitrate(&radio, RADIO_BITRATE_BPS) != RFM69_OK) return 1;
    if (rfm69_set_deviation_hz(&radio, RADIO_DEVIATION_HZ) != RFM69_OK) return 1;
    if (rfm69_set_rx_bandwidth_hz(&radio, RADIO_RX_BANDWIDTH_HZ) != RFM69_OK) return 1;
    /* Two receive-front-end registers this init never named, so both sat at
     * their reset values for the whole of the radio's life. Measured floor is
     * -108 dBm with bursts to -79, so a threshold below the floor is a level
     * condition that is permanently true. */
    if (rfm69_set_rssi_threshold_dbm(&radio, -100) != RFM69_OK) return 1;
    if (rfm69_set_dagc(&radio, 0) != RFM69_OK) return 1;
    if (rfm69_set_carrier_hz(&radio, slot_hz(RADIO_JOIN_SLOT)) != RFM69_OK) return 1;
    if (rfm69_set_modulation(&radio, RFM69_SHAPING_BT_0_5) != RFM69_OK) return 1;
    if (rfm69_set_preamble_bytes(&radio, 4) != RFM69_OK) return 1;
    if (rfm69_set_sync(&radio, sync_val, sizeof(sync_val), 0) != RFM69_OK) return 1;

    /* Whitening, not Manchester: the old setting doubled the time on air for
     * the same DC balance, which is most of why the hub sat at 3.5% duty. */
    /* No whitening: the SX1231 and the device's SX126x use different LFSR
     * conventions, so matching them is a silent-failure risk for no gain -
     * unlike Manchester it costs no air time either way, and the payload is
     * AEAD ciphertext that is already DC balanced. */
    if (rfm69_set_packet_format(&radio, 1, RFM69_DCFREE_NONE, 1,
                                RFM69_FILTER_NONE) != RFM69_OK) return 1;
    /* CrcAutoClearOff. The CRC is still computed and still reported; what
     * changes is that a frame failing it is *delivered* instead of dropped
     * before PayloadReady. Without this the hub cannot tell an empty band from
     * a corrupted frame - the part discards silently and no counter anywhere
     * moves, which is exactly the blindness that made two candidate faults
     * produce identical evidence during the first on-air attempts.
     *
     * Set by hand because the library's set_packet_format does not expose bit
     * 3, and writing the whole register here would silently undo whatever that
     * call configured the moment either changes. */
    {
        uint8_t pc1 = 0;

        if (rfm69_read_reg(&radio, RFM69_RegPacketConfig1, &pc1) != RFM69_OK)
            return 1;
        if (rfm69_write_reg(&radio, RFM69_RegPacketConfig1,
                            (uint8_t)(pc1 | 0x08u)) != RFM69_OK)
            return 1;
    }
    if (rfm69_set_payload_length(&radio, RFM69_FIFO_SIZE - 2) != RFM69_OK) return 1;
    /* Set but inert: filtering is FILTER_NONE above. Kept because authentication,
     * not addressing, is what rejects a frame - filtering would only be a power
     * optimisation, and enabling it would claim the first payload byte that the
     * frame type now occupies. */
    if (rfm69_set_node_address(&radio, node_id) != RFM69_OK) return 1;
    if (rfm69_set_broadcast_address(&radio, BROADCAST_ADDR) != RFM69_OK) return 1;
    /* PA1, not PA0. This module's PA0 pin is not bonded to the antenna, so the
     * hub radiated about -40 dBm against a +13 dBm setting - measured by the
     * device's receiver, by reciprocity and by the SDR, all agreeing. */
    if (rfm69_set_power(&radio, RFM69_PA1, 13) != RFM69_OK) return 1;

    if (rfm69_run_osc_calibration(&radio, 50000u) != RFM69_OK) return 1;
    if (rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US) != RFM69_OK)
        return 1;

    if (hop_init(&hop, hop_prf_aes, NULL, RADIO_HOP_COUNT) != 0)
        return 1;

    /* Before the grid starts. A radio that cannot seal a frame correctly should
     * not transmit one, and the two published frames go through the same CRYP
     * path the exchange will use rather than a round trip against itself - only
     * matching the vector proves the nonce and AAD are assembled the way the
     * far side assembles them. */
    aead_selftest_rc = aead_selftest();
    /* After the frame cipher, deliberately: it leaves CRYP configured for GCM
     * with a byte data width, and the PRF must be correct from that state
     * rather than from a freshly initialised one. */
    if (aead_selftest_rc == 0 && hop_prf_selftest() != 0)
        aead_selftest_rc = -20;
    if (aead_selftest_rc == 0 && frame_selftest() != 0)
        aead_selftest_rc = -30;

    /* Not the network's key, and not zeros - see hop_key_placeholder(). */
    hop_key_placeholder();

    /* Never restart the protocol's clock at zero. Everything at or below the
     * stored ceiling was reserved by a previous boot, and reusing it under a
     * persisted session key repeats a GCM nonce. */
    frame_counter = kv_reserved();
    /* Reserve before the grid starts, so the very first beacon is already
     * inside what flash guarantees rather than costing a superframe of
     * silence to get there. */
    (void)kv_reserve(frame_counter);

    delay_ms_it(SUPERFRAME_US / 1000u);
    return 0;
}

/* Builds the frame in place. The length byte is the payload size, which is what
 * the RFM69 sends in variable-length mode. */
static uint8_t build_frame(const void *payload, uint8_t payload_len) {
    if ((size_t)payload_len + 1u > sizeof(tx_buffer))
        return 0;
    tx_buffer[0] = payload_len;
    memcpy(tx_buffer + 1, payload, payload_len);
    return (uint8_t)(payload_len + 1u);
}

static rfm69_status_t transmit(uint8_t total_len) {
    rfm69_status_t st;

    /* Out of RX before the FIFO is touched. Writing it while the receiver is
     * running mixes the outgoing frame with whatever the packet engine has
     * collected and the transmit fails - measured as tx err on the join beacon
     * once, fixed at that call site, and then hit again from pair_tx where the
     * failure lost a PAIR_RSP. It belongs here, where no caller can omit it. */
    st = rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
    if (st != RFM69_OK) return st;

    st = rfm69_set_fifo_threshold(&radio, 1, 0);
    if (st != RFM69_OK) return st;
    st = rfm69_write_fifo(&radio, tx_buffer, total_len);
    if (st != RFM69_OK) return st;

    st = rfm69_set_mode(&radio, RFM69_MODE_TX);
    if (st != RFM69_OK) return st;

    /* PacketSent from RegIrqFlags2 rather than polling DIO0 forever. */
    st = rfm69_wait_irq2(&radio, RFM69_IRQ2_PACKET_SENT, TX_TIMEOUT_US);
    (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
    return st;
}

/* The superframe boundary is an absolute grid: the next one is a fixed step from
 * the last boundary, never from whenever the work finished. Adding the period to
 * "now" lets transmit time accumulate as drift, which is why the measured
 * superframe was 2004 ms rather than 2000 - and a drifting grid is one a device
 * cannot predict a slot on.
 *
 * The counter advances here and nowhere else, independent of whether anything was
 * transmitted. Tying it to transmit success stalls the protocol's clock on a
 * radio error and repeats a superframe number, which once frames are sealed is
 * nonce reuse. */
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

    /* Re-read after the boundary, so a refined measurement moves the next
     * interval and never the one already being timed. */
    superframe_tk = timebase_us_to_ticks(SUPERFRAME_US);
    join_offset_tk = timebase_us_to_ticks(RADIO_JOIN_OFFSET_US);

    /* If something stalled us past a whole period, step the grid forward rather
     * than transmitting a burst to catch up: it is a schedule, not a queue. */
    while (timebase_elapsed(superframe_start_tk + superframe_tk)) {
        superframe_start_tk += superframe_tk;
        frame_counter++;
    }
    return 1;
}


static void RFM_send_broadcast(uint8_t flags, uint8_t resume_in) {
    radio_data_beacon_t payload;
    uint8_t total;
    uint8_t hop_idx;

    /* How far past the boundary this beacon actually leaves. A device measuring
     * the period from consecutive beacons inherits this jitter directly, so it
     * is measured rather than assumed to be small. */
    late_last_us = rfm_micros() - superframe_start_tk;
    if (late_last_us > late_max_us) late_max_us = late_last_us;
    if (late_last_us < late_min_us) late_min_us = late_last_us;
    if (late_last_us > timebase_us_to_ticks(RADIO_BEACON_LATE_LIMIT_US))
        late_over++;

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

    total = build_frame(&payload, (uint8_t)sizeof(payload));
    if (total == 0) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_BUILD;
        return;
    }

    /* One hop per superframe, ordered by a keyed shuffle rather than counting
     * upwards. A linear sweep puts consecutive bursts 100 kHz apart, so one
     * wideband interferer takes out several frames in a row, and two observed
     * bursts give away every future channel.
     *
     * A PRF failure means silence, not a guess: transmitting on a channel no
     * device can compute is worse than not transmitting. */
    if (hop_channel(&hop, frame_counter, &hop_idx) != 0) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_PRF;
        return;
    }
    if (rfm69_set_carrier_hz(&radio,
            slot_hz(hop_slot_to_grid(hop_idx))) != RFM69_OK) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_RETUNE;
        return;
    }
    if (transmit(total) != RFM69_OK) {
        beacon_err++;
        beacon_err_last = RADIO_BERR_TX;
    }
}

/* Fixed channel, cleartext, no key needed to read it. This is the one frame a
 * device can act on before it has been paired, so it carries nothing secret -
 * only what a joiner needs to find the network and align its counter. */
static void RFM_send_join_beacon(void) {
    radio_join_beacon_t payload;
    uint8_t total;

    payload.type         = RADIO_FRAME_JOIN_BEACON;
    payload.version      = RADIO_PROTO_VERSION;
    payload.net_id       = RADIO_NET_ID;
    payload.hub_id       = hub_id;
    payload.superframe   = frame_counter;
    payload.flags        = RADIO_JOIN_FLAG_WINDOW_OPEN;
    payload.hop_channels = RADIO_HOP_COUNT;

    total = build_frame(&payload, (uint8_t)sizeof(payload));
    if (total == 0)
        return;
    if (rfm69_set_carrier_hz(&radio, slot_hz(RADIO_JOIN_SLOT)) != RFM69_OK) {
        join_tx_err++;
        return;
    }
    join_beacons++;
    if (transmit(total) != RFM69_OK)
        join_tx_err++;
}

/* One request, one reply, always: a caller waiting on a sequence number must
 * not be left waiting because a request type was unhandled. */
static void RFM_serve_request(const ipc_msg_t *req) {
    uint8_t status = IPC_ST_OK;
    uint8_t reply = 0;
    uint8_t len = 0;

    switch (req->type) {
    case IPC_REQ_READ_REG:
        if (rfm69_read_reg(&radio, req->payload[0], &reply) != RFM69_OK)
            status = IPC_ST_RADIO_ERR;
        len = 1;
        break;
    case IPC_REQ_ADD_DEVICE: {
        uint32_t dev_id;

        if (req->len < 1u + sizeof(dev_id)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&dev_id, &req->payload[1], sizeof(dev_id));
        reply = RFM_open_pairing(dev_id);
        len = 1;
        break;
    }
    case IPC_REQ_GET_TIMING: {
        ipc_timing_t t;

        t.superframe   = frame_counter;
        t.now_tk       = rfm_micros();
        t.late_last_us = late_last_us;
        t.late_max_us  = late_max_us;
        t.late_min_us  = (late_min_us == 0xFFFFFFFFu) ? 0 : late_min_us;
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
        t.late_over     = late_over;
        (void)ipc_send_reply(req, IPC_ST_OK, &t, (uint8_t)sizeof(t));
        return;
    }
    /* Runs the real hop PRF on a caller-supplied block, so what the accelerator
     * actually computes can be compared against a host AES rather than argued
     * from the reference manual. */
    case IPC_REQ_HOP_PRF: {
        uint8_t in[16], out[16];

        if (req->len < 1u + sizeof(in)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(in, &req->payload[1], sizeof(in));
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
    /* Test scaffolding. The quiesce is otherwise only reachable by a real
     * device transmitting a valid PAIR_REQ, and the thing worth verifying with
     * an SDR - that the hub goes silent for exactly the announced number of
     * superframes and comes back on the one it promised - does not need a
     * device to be true. */
    case IPC_REQ_QUIESCE:
        if (req->len < 1u) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        reply = begin_quiesce(req->payload[0]);
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
    /* CM7 replaying its store into a freshly booted radio core. Without it a
     * paired device is unreachable after any hub reset: the keys live in CM7's
     * flash and nothing here survives power. */
    case IPC_REQ_INSTALL_DEVICE: {
        ipc_device_keys_t k;

        if (req->len < 1u + sizeof(k)) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        memcpy(&k, &req->payload[1], sizeof(k));
        if (install_device(&k) != 0)
            status = IPC_ST_BAD_ARG;
        len = 0;
        break;
    }
    case IPC_REQ_SET_REPORT_RATE:
        if (req->len < 2u) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        report_every_grant = (req->payload[1] == 0u) ? 1u : req->payload[1];
        reply = report_every_grant;
        len = 1;
        break;
    case IPC_REQ_GET_RXDIAG: {
        ipc_rx_diag_t d;

        memset(&d, 0, sizeof(d));
        d.sync_match      = rx_sync_match;
        d.crc_err         = rx_crc_err;
        d.frames          = rx_frames;
        d.last_superframe = rx_last_superframe;
        d.last_len        = rx_last_len;
        d.last_type       = rx_last_type;
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
    /* The FIFO is the only writable block this part has that is the same width
     * as a frame, so a pattern through it exercises the exact read that
     * delivers a payload - without needing anything on air. */
    case IPC_REQ_SPI_LOOP: {
        ipc_spi_loop_t r;
        uint8_t pattern[57], back[57];
        uint32_t n = 0, i, pass;

        memset(&r, 0, sizeof(r));
        if (req->len >= 4) memcpy(&n, req->payload, 4);
        if (n == 0u || n > 2000u) n = 200u;

        /* Alternating high-bit bytes: a cleared or set bit 7 shows in both
         * directions, which a pattern of one polarity could not distinguish. */
        for (i = 0; i < sizeof(pattern); i++)
            pattern[i] = (i & 1u) ? 0xA5u : 0x5Au;

        (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);

        /* RegAesKey holds 16 bytes verbatim and nothing reads it - AesOn is 0.
         * A register block round-trips by definition, so a difference here is
         * the bus and a difference only in the FIFO is the FIFO. */
        for (pass = 0; pass < n; pass++) {
            uint8_t rb[16];

            if (rfm69_write(&radio, RFM69_RegAesKey1, pattern, 16) != RFM69_OK ||
                rfm69_read(&radio, RFM69_RegAesKey1, rb, 16) != RFM69_OK) {
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
            if (rfm69_write_fifo(&radio, pattern, sizeof(pattern)) != RFM69_OK ||
                rfm69_read_fifo(&radio, back, sizeof(back)) != RFM69_OK) {
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
            /* Both halves measured rather than derived: the kernel clock from
             * RCC, the divider from CFG1, where MBR divides by 2^(mbr+1). A
             * prescaler quoted without its kernel clock says nothing - the PLL
             * that sets it is three files away and on the other core. */
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

        if (req->len >= 5u) memcpy(&sf, &req->payload[1], 4);
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

        /* payload[1], not payload[0]: hub_ipc_call puts its `arg` byte first so
         * every request has one shape on the wire. Reading from 0 shifts the
         * whole struct by a byte - which is the GET_DEVICE_INFO defect, in the
         * same file, found the same evening. */
        if (req->len < 1u + sizeof(p)) { status = IPC_ST_BAD_ARG; break; }
        memcpy(&p, &req->payload[1], sizeof(p));
        if (p.len == 0u || p.len > sizeof(pi_frame) || p.superframe == 0u) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        /* An unsent frame being displaced is worth counting rather than
         * silently dropping: it means CM7 is building faster than the grid
         * transmits, which is a schedule disagreement and not a radio fault. */
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
        d.prf_err       = dl_prf_err;
        d.last_hz       = dl_last_hz;
        d.last_superframe = dl_last_sf;
        d.next_slot     = dl_next_slot;
        (void)ipc_send_reply(req, IPC_ST_OK, &d, (uint8_t)sizeof(d));
        return;
    }
    case IPC_REQ_GET_VECTORS: {
        ipc_vectors_t v;

        memset(&v, 0, sizeof(v));
        /* Compiled into this binary, so what comes back is what this core was
         * actually built with - not what the tree says today. */
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
        x.uplink_frames   = up_frames;
        x.uplink_ok       = up_ok;
        x.uplink_bad_slot = up_bad_slot;
        x.uplink_bad_frame = up_bad_frame;
        x.uplink_bad_tag  = up_bad_tag;
        x.uplink_replay   = up_replay;
        (void)ipc_send_reply(req, IPC_ST_OK, &x, (uint8_t)sizeof(x));
        return;
    }
    /* The index-th live device. `total` rides along so the console learns how
     * many there are from the first answer instead of probing until refused. */
    case IPC_REQ_GET_DEVICE_INFO: {
        ipc_device_report_t d;
        uint32_t seen = 0;
        uint32_t want;
        uint32_t i;

        /* The index is the request's arg byte, which hub_ipc_call puts at
         * payload[0]; payload[1] onwards is a caller's own data and this call
         * sends none. Reading it there made every listing fail with BAD_ARG,
         * and nothing noticed because it takes a paired device to run. */
        if (req->len < 1u) {
            status = IPC_ST_BAD_ARG;
            break;
        }
        want = req->payload[0];
        for (i = 0; i < RADIO_MAX_DEVICES; i++) {
            if (!devices[i].used)
                continue;
            if (seen++ != want)
                continue;

            memset(&d, 0, sizeof(d));
            d.dev_id          = devices[i].dev_id;
            d.last_superframe = devices[i].last_superframe;
            d.frames_ok       = devices[i].frames_ok;
            d.frames_bad      = devices[i].frames_bad;
            d.uptime_s        = devices[i].uptime_s;
            d.total           = device_count;
            d.slot            = devices[i].slot;
            d.rssi_up         = devices[i].rssi_up;
            d.rssi_down       = devices[i].rssi_down;
            d.supply_mv       = devices[i].supply_mv;
            d.report_every    = devices[i].report_every;
            d.flags           = devices[i].flags;
            (void)ipc_send_reply(req, IPC_ST_OK, &d, (uint8_t)sizeof(d));
            return;
        }
        status = IPC_ST_BAD_ARG;
        break;
    }
    case IPC_REQ_REMOVE_DEVICE:
    case IPC_REQ_SET_DEVICE_PARAM:
    default:
        status = IPC_ST_UNKNOWN_REQ;
        break;
    }

    (void)ipc_send_reply(req, status, &reply, len);
}


/* The superframe counter is the protocol's clock and keeps advancing through a
 * quiesce. Only transmission stops.
 *
 * That distinction is the whole reason a quiesce is cheap here: the hop
 * sequence is *indexed* by the counter rather than stepped by it, so a device
 * that slept through the silence computes the channel for the superframe it
 * wakes into and is simply back. Freezing the counter instead would repeat
 * superframe numbers, which once frames are sealed is nonce reuse. */
static void on_superframe(void) {
    /* Past what flash guarantees, so a frame sent now would carry a counter a
     * future boot will issue again - and a repeated GCM nonce leaks the
     * authentication subkey, not merely the plaintext. Silence is the only safe
     * answer. The counter still advances: it is the clock, and stalling it
     * would repeat superframe numbers, which is the same defect by the other
     * route. Reachable only when the flash log is full, which `rfm store`
     * reports as an error. */
    if (!kv_counter_safe(frame_counter)) {
        unreserved_frames++;
        return;
    }

    if (pair_state == RADIO_PAIR_QUIESCE) {
        int32_t left = (int32_t)(quiesce_resume_at - frame_counter);

        if (left > 0) {
            /* Repeat the announcement, counting down, so a device that missed
             * the first copy still derives the same resume superframe from the
             * second. After the announce run the hub is silent here and parked
             * on the join channel. */
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
        /* Announce and commit in the same breath. The resume superframe is
         * fixed here and never moved afterwards: devices sleep against it, so
         * extending a quiesce would strand every one of them at a moment when
         * none of them is listening. An exchange that overruns loses its
         * window - the network does not lose its schedule. */
        quiesce_resume_at = frame_counter + quiesce_len;
        pair_state = RADIO_PAIR_QUIESCE;
        RFM_send_broadcast(RADIO_BEACON_FLAG_QUIESCE, quiesce_len);
        return;
    }

    RFM_send_broadcast(0, 0);
}

/* Only the device the operator named may move the state machine. A quiesce is
 * eight seconds of network silence, so triggering one from any frame that
 * arrives on an open join channel would hand a denial of service to anyone in
 * range - and the join channel is fixed and published. */
/* --- the four-frame exchange ------------------------------------------- */

/* A frame that failed CRC still occupies the FIFO once CrcAutoClearOff is set,
 * so it has to be read out or it becomes the head of the next one. */
static void rx_discard_frame(void) {
    uint8_t len = 0;

    if (rfm69_read_fifo(&radio, &len, 1) != RFM69_OK)
        return;
    if (len == 0u || len > sizeof(rx_buffer))
        return;
    (void)rfm69_read_fifo(&radio, rx_buffer, len);
}

/* 1 when a frame is ready and its CRC verified. Counts and drains the frames
 * that failed, which is the whole point of taking them off the hardware's
 * silent-discard path. */
/* SyncAddressMatch is a level, not a pulse - it stays asserted until the mode
 * changes - so an edge is what a frame is. Polling the level would count one
 * arrival hundreds of times and produce a number that looks like a busy,
 * healthy channel. */
/* Sampled every superloop pass while the receiver is open, and every sample is
 * triggered: RegRssiValue is a latch the part only refills on request, so the
 * poll this replaced reported the level at RX entry for the whole window. Peak
 * and floor were therefore always equal and always looked like a quiet band. */
static void rx_sample_rssi(void) {
    int16_t x2 = 0;

    if (rfm69_measure_rssi(&radio, RSSI_TIMEOUT_US, &x2) != RFM69_OK)
        return;
    rx_rssi_samples++;
    /* The register counts half-dB *below* zero and the driver returns it
     * negated, so a stronger signal is a larger (less negative) number. */
    if (x2 > rx_rssi_peak_x2)  rx_rssi_peak_x2  = x2;   /* loudest */
    if (x2 < rx_rssi_floor_x2) rx_rssi_floor_x2 = x2;   /* quietest */
}

static void rx_note_sync(uint8_t flags1) {
    uint8_t now = (flags1 & RFM69_IRQ1_SYNC_ADDR_MATCH) ? 1u : 0u;

    if (now && !sync_was_set)
        rx_sync_match++;
    sync_was_set = now;
}

static int rx_frame_ready(uint8_t flags2) {
    if (!(flags2 & RFM69_IRQ2_PAYLOAD_READY))
        return 0;
    if (!(flags2 & RFM69_IRQ2_CRC_OK)) {
        rx_crc_err++;
        rx_discard_frame();
        return 0;
    }
    rx_frames++;
    return 1;
}

static void ex_reset(void) {
    ex_state   = RADIO_EX_IDLE;
    ex_dev_id  = 0;
    ex_waiting = 0;
    ex_retry   = 0;
    memset(&ex_rsp, 0, sizeof(ex_rsp));
    memset(&ex_keys, 0, sizeof(ex_keys));
}

/* The channel is an argument because it was once a constant inside here: this
 * function tuned the join channel itself, and the downlink reusing it keyed 93
 * frames on 866.5 MHz while the device listened on the hop channel. Every
 * counter read success, because they all did transmit. */
static int frame_tx(const void *payload, uint8_t len, uint32_t hz) {
    uint8_t total = build_frame(payload, len);

    if (total == 0)
        return -1;
    if (rfm69_set_carrier_hz(&radio, hz) != RFM69_OK)
        return -1;
    if (transmit(total) != RFM69_OK)
        return -1;
    /* Back to listening immediately: the device answers as soon as it has
     * finished its own curve work, and a receiver left in standby loses the
     * frame with no error anywhere. */
    (void)rfm69_set_mode(&radio, RFM69_MODE_RX);
    return 0;
}

/* Named for its channel, not its caller: every pairing frame goes out here. */
static int pair_tx(const void *payload, uint8_t len) {
    return frame_tx(payload, len, slot_hz(RADIO_JOIN_SLOT));
}

/* Split out so the self-test can compare what actually transmits against the
 * published frame. The device side sized a sealed body as grant + hop_key when
 * the grant already contained the hop key - 35 bytes instead of 19, tag failure
 * on a frame with nothing wrong, and on air indistinguishable from a radio
 * fault. A self-test built on a frame the same side assembled would have agreed
 * with the mistake perfectly. The mirror of that hole was here: nothing
 * compared these builders to anything. */
static void build_pair_rsp(radio_pair_rsp_t *f, uint32_t hid, uint32_t did,
                           const uint8_t eph[33], const uint8_t confirm[16]) {
    f->type    = RADIO_FRAME_PAIR_RSP;
    f->version = RADIO_PROTO_VERSION;
    f->hub_id  = hid;
    f->dev_id  = did;
    memcpy(f->eph_pubkey, eph, 33);
    memcpy(f->confirm, confirm, 16);
}

static void build_pair_accept_hdr(radio_pair_accept_t *f, uint32_t hid, uint32_t did,
                                  uint32_t superframe, uint8_t retry) {
    f->type       = RADIO_FRAME_PAIR_ACCEPT;
    f->version    = RADIO_PROTO_VERSION;
    f->hub_id     = hid;
    f->dev_id     = did;
    f->superframe = superframe;
    f->retry      = retry;
}

/* Every frame this core assembles, against the published bytes. Field order,
 * endianness and the AAD offset all at once - and against bytes the far side
 * also compiles, which is the only thing that makes it a check rather than
 * this core agreeing with itself. */
static int frame_selftest(void) {
    radio_pair_rsp_t rsp;
    radio_pair_accept_t acc;

    build_pair_rsp(&rsp, 0x33442211u, 0x0000002Au,
                   PV_FRAME_RSP + 10, PV_FRAME_RSP + 43);
    if (memcmp(&rsp, PV_FRAME_RSP, sizeof(rsp)) != 0)
        return -1;

    /* Only the cleartext header, because that is what this builder owns - and
     * it is also the AAD, so a field that moved would break every tag with
     * byte-perfect ciphertext. */
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

/* The slot grant, sealed under the session key the exchange just produced.
 * The network hop key travels inside it: it cannot be derived from the pairing
 * secret, which is pairwise, and it must not travel in clear. */
static void send_pair_accept(void) {
    radio_pair_accept_t f;
    radio_pair_grant_t grant;
    uint8_t nonce[AEAD_NONCE_BYTES];

    build_pair_accept_hdr(&f, hub_id, ex_dev_id, frame_counter, ex_retry);

    grant.slot         = ex_keys.slot;
    grant.report_every = ex_keys.report_every;
    grant.flags        = 0;
    memcpy(grant.hop_key, ex_keys.hop_key, sizeof(grant.hop_key));

    /* The slot field of the nonce cannot be the slot being granted: that value
     * is inside the sealed plaintext, and the device needs the nonce to open
     * it. The reserved range plus a retry index is what keeps this frame's
     * nonces distinct from every slotted frame's and from each other. */
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

/* Installs, or replaces, one device. Replacing matters: a device that re-pairs
 * keeps its slot, and leaving the old session key in place would make every
 * frame it sends under the new one fail its tag. */
static int install_device(const ipc_device_keys_t *k) {
    dev_entry_t *d;

    if (k->slot >= RADIO_MAX_DEVICES || k->dev_id == 0u)
        return -1;

    d = &devices[k->slot];
    if (!d->used)
        device_count++;
    memset(d, 0, sizeof(*d));
    d->used         = 1;
    d->slot         = k->slot;
    d->dev_id       = k->dev_id;
    d->key_gen      = k->key_gen;
    d->report_every = (k->report_every == 0u) ? 1u : k->report_every;
    memcpy(d->session_key, k->session_key, sizeof(d->session_key));

    /* Seed the replay floor from the superframe counter, which is already
     * durable and already monotone: kvstore reserves a ceiling ahead, so a
     * reboot resumes at or above where it left off and never below.
     *
     * Without this the floor started at zero and the first frame of every boot
     * was accepted unconditionally - the guard consulted only after an
     * acceptance in the same session, which is exactly the moment it exists
     * for, since a reboot is what an attacker with a recording arranges.
     * Every recorded frame is from a superframe below this, so it is refused
     * before it can move the floor.
     *
     * No new flash field: the durability comes from the counter, which had to
     * be durable anyway to stop GCM nonces repeating. Found by applying the
     * device side's own ever_accepted correction to this path. */
    d->rx_floor = frame_counter;

    /* The hop key is network-wide, so every install carries the same 16 bytes.
     * A change means the cached permutation was computed under the old key and
     * has to be thrown away - hop_channel caches one cycle, about a minute, and
     * would otherwise keep answering from it while the network moved on. */
    if (!net_hop_key_set || memcmp(net_hop_key, k->hop_key, sizeof(net_hop_key)) != 0) {
        memcpy(net_hop_key, k->hop_key, sizeof(net_hop_key));
        net_hop_key_set = 1;
        if (hop_init(&hop, hop_prf_aes, NULL, RADIO_HOP_COUNT) != 0)
            return -1;
    }
    return 0;
}

static void exchange_service(void) {
    ipc_msg_t reply;

    if (ex_state == RADIO_EX_IDLE || ex_state == RADIO_EX_ACCEPTED)
        return;

    if (ex_waiting && ipc_poll_event_reply(ex_seq, &reply)) {
        ex_waiting = 0;
        if (reply.status != IPC_ST_OK) {
            /* CM7 refused: an unenrolled device, a fingerprint that does not
             * match, a repeated nonce, a confirmation that did not verify.
             * Every one of those is a reason not to continue, and none of them
             * is a reason to retry with the same inputs. */
            ex_cm7_refused++;
            ex_reset();
            return;
        }
        if (ex_state == RADIO_EX_WAIT_RSP) {
            if (reply.len < sizeof(ex_rsp)) { ex_cm7_refused++; ex_reset(); return; }
            memcpy(&ex_rsp, reply.payload, sizeof(ex_rsp));
            send_pair_rsp();
            return;
        }
        if (ex_state == RADIO_EX_WAIT_KEYS) {
            if (reply.len < sizeof(ex_keys)) { ex_cm7_refused++; ex_reset(); return; }
            memcpy(&ex_keys, reply.payload, sizeof(ex_keys));
            /* Installed before the grant is transmitted. A device that opens
             * PAIR_ACCEPT and starts reporting into a hub that has not yet
             * installed its key would have every frame refused, and the hub
             * would be the one at fault. */
            if (install_device(&ex_keys) != 0) { ex_reset(); return; }
            ex_paired++;
            send_pair_accept();
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
    (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
    sync_was_set = 0;      /* the flag clears with the mode; so must its shadow */
    uplink_rx_open = 0;
}

static void handle_uplink_frame(void) {
    radio_uplink_t f;
    radio_uplink_report_t rpt;
    uint8_t nonce[AEAD_NONCE_BYTES];
    dev_entry_t *d;
    int16_t rssi_x2 = 0;
    uint8_t len = 0;

    if (rfm69_read_fifo(&radio, &len, 1) != RFM69_OK)
        return;
    if (len == 0u || len > sizeof(rx_buffer))
        return;
    if (rfm69_read_fifo(&radio, rx_buffer, len) != RFM69_OK)
        return;

    /* Taken before anything else can retune or change mode. It is the hub's
     * half of the link measurement and there is no second chance at it. */
    (void)rfm69_get_rssi(&radio, &rssi_x2);

    up_frames++;
    if (len < sizeof(f) || rx_buffer[0] != RADIO_FRAME_UPLINK ||
        rx_buffer[1] != RADIO_PROTO_VERSION) {
        up_bad_frame++;
        return;
    }
    memcpy(&f, rx_buffer, sizeof(f));

    /* No device id on the wire: the hub assigned the slot, so it owns the map.
     * A frame naming an unassigned slot costs no crypto at all. */
    if (f.slot >= RADIO_MAX_DEVICES || !devices[f.slot].used) {
        up_bad_slot++;
        return;
    }
    d = &devices[f.slot];

    aead_nonce(nonce, f.superframe, d->dev_id, RADIO_DIR_UPLINK, f.slot);
    if (aead_open(d->session_key, nonce, (const uint8_t *)&f, RADIO_UPLINK_AAD_LEN,
                  f.ct, sizeof(f.ct), (uint8_t *)&rpt, f.tag) != 0) {
        up_bad_tag++;
        d->frames_bad++;
        return;
    }

    /* After the tag, never before. A frame that has not authenticated must not
     * be able to move the floor - that would let one forgery lock out every
     * genuine frame behind it, turning a replay guard into a denial of service.
     *
     * Strictly increasing, signed so it survives the counter wrap. This is the
     * The floor is seeded at install from the durable counter rather than from
     * zero, so there is no "first frame of the session" that bypasses it.
     *
     * check rx_floor was named for in the keystore record and that nothing had
     * ever performed: the field was written as zero in three places and read in
     * none, so every uplink frame ever received here was replayable with a
     * valid tag, counted as frames_ok, and overwrote the device's telemetry
     * with whatever the recording held. */
    if ((int32_t)(f.superframe - d->rx_floor) <= 0) {
        up_replay++;
        d->frames_replay++;
        return;
    }
    d->rx_floor = f.superframe;

    up_ok++;
    d->frames_ok++;
    d->last_superframe = f.superframe;
    d->rssi_up   = (int8_t)(rssi_x2 / 2);
    d->rssi_down = rpt.rssi_down;
    d->flags     = rpt.flags;
    d->supply_mv = rpt.supply_mv;
    d->uptime_s  = rpt.uptime_s;
}

/* Open for the whole uplink region rather than per slot, and closed before the
 * join region and before the next boundary - a receive in progress when the
 * beacon is due puts that delay into the number every device measures its
 * period from. */
/* The hub's half of the periodic exchange. Sent in the downlink region, on the
 * hop channel the beacon has already left the radio tuned to, at half rate
 * because beacon + downlink + join beacon every superframe is 1.42% and over
 * the ETSI limit.
 *
 * Sealed under one device's session key, so unlike the data beacon it is
 * authenticated. That is the reason it is worth a region: a forger cannot
 * command a device without the key pairing established. */
/* Keyed on the join channel, at the superframe CM7 named, while a window is
 * open.
 *
 * **It replaces the join beacon on that superframe**, and the replacement is
 * load-bearing rather than a duty-cycle nicety. The invitation cadence is 4 and
 * the beacon's is 2, so 4 being a multiple of 2 means every invitation shares a
 * superframe with a beacon - not sometimes, always - and both were keyed at the
 * same offset, back to back. The device heard 15 beacons and zero invitations
 * in one 59 s window through the same receiver, every invitation hidden in the
 * beacon it followed by 8 ms.
 *
 * A collision that happens on every occurrence looks like a frame that never
 * radiates, and the transmitter cannot tell the two apart: carrier read back
 * correct, PacketSent observed, tx_err zero.
 *
 * It replaces the join beacon rather than joining it: both are the hub
 * saying "here I am" on 866.5, and only one of them is addressed and
 * authenticated. See ADR-0021.
 *
 * A frame whose superframe has already passed is dropped rather than sent late.
 * The device aligns its counter from this field, so a stale one hands it a
 * counter that is wrong by exactly the amount it was delayed - worse than no
 * invitation, because it looks like a successful one. */
static void pair_init_service(void) {
    if (pi_superframe == 0u || pi_len == 0u)
        return;
    if (!grid_started || pair_state != RADIO_PAIR_LISTEN)
        return;

    if ((int32_t)(frame_counter - pi_superframe) > 0) {
        pi_missed++;
        pi_superframe = 0u;
        pi_len = 0u;
        return;
    }
    if (frame_counter != pi_superframe)
        return;
    /* Same offset the join beacon has always used, and for the same reason: it
     * is where a device that is listening has been told to listen. */
    if (!timebase_elapsed(superframe_start_tk + join_offset_tk))
        return;

    pi_superframe = 0u;
    if (frame_tx(pi_frame, pi_len, slot_hz(RADIO_JOIN_SLOT)) != 0)
        pi_tx_err++;
    else {
        pi_sent++;
        pi_last_sent_sf = frame_counter;
    }
    /* Read back what the part actually holds, straight after the transmit.
     * frame_tx returning 0 means PacketSent was observed; it is not evidence
     * the carrier or the length were what was asked for, and the device hears
     * 15 join beacons and none of these through the same receiver. A driver
     * call that returns success is not evidence a field holds what was set. */
    {
        uint8_t v = 0;

        pi_frf = 0;
        if (rfm69_read_reg(&radio, RFM69_RegFrfMsb, &v) == RFM69_OK)
            pi_frf |= (uint32_t)v << 16;
        if (rfm69_read_reg(&radio, RFM69_RegFrfMid, &v) == RFM69_OK)
            pi_frf |= (uint32_t)v << 8;
        if (rfm69_read_reg(&radio, RFM69_RegFrfLsb, &v) == RFM69_OK)
            pi_frf |= v;
        if (rfm69_read_reg(&radio, RFM69_RegPayloadLength, &v) == RFM69_OK)
            pi_paylen = v;
    }
    pi_len = 0u;
}

static void downlink_service(void) {
    radio_downlink_t f;
    radio_downlink_cmd_t body;
    uint8_t nonce[AEAD_NONCE_BYTES];
    dev_entry_t *d = NULL;
    uint8_t i, slot, hop_idx;

    if (!grid_started || pair_state == RADIO_PAIR_QUIESCE || device_count == 0u)
        return;
    if (!RADIO_DOWNLINK_ON(frame_counter))
        return;
    if (dl_served == frame_counter)
        return;
    if (!timebase_elapsed(superframe_start_tk +
                          timebase_us_to_ticks(RADIO_DOWNLINK_RX_OPEN_US)))
        return;
    /* Past the region is not late, it is a different region: the first uplink
     * slot opens at the close and a hub still keying there collides with the
     * device it is talking to. */
    if (timebase_elapsed(superframe_start_tk +
                         timebase_us_to_ticks(RADIO_DOWNLINK_RX_CLOSE_US)))
        return;

    dl_served = frame_counter;
    dl_opportunities++;

    /* Round robin from wherever the last one stopped. Scanned rather than
     * indexed because slots are sparse - a removed device leaves a hole, and
     * indexing past it would serve nobody on that opportunity. */
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
    body.cmd        = RADIO_CMD_NOP;
    /* A superframe is SUPERFRAME_US of nominal time, so seconds is a multiply,
     * not a divide. The first version of this line divided by 500 and would
     * have shipped a field that is wrong by 1000x - and nothing on either side
     * checks it, because it is advisory. */
    body.hub_time_s = frame_counter * (SUPERFRAME_US / 1000000u);

    f.type       = RADIO_FRAME_DOWNLINK;
    f.version    = RADIO_PROTO_VERSION;
    f.slot       = d->slot;
    f.superframe = frame_counter;

    aead_nonce(nonce, f.superframe, d->dev_id, RADIO_DIR_DOWNLINK, f.slot);
    if (aead_seal(d->session_key, nonce, (const uint8_t *)&f, RADIO_DOWNLINK_AAD_LEN,
                  (const uint8_t *)&body, sizeof(body), f.ct, f.tag) != 0) {
        dl_seal_err++;
        return;
    }
    /* The hop channel of this superframe, computed here rather than inherited
     * from whatever the beacon left tuned - the beacon can fail its retune and
     * still leave a plausible carrier behind. A PRF failure is silence, for the
     * same reason it is in the beacon: a frame on a channel the device cannot
     * compute is worse than no frame. */
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
}

static void uplink_service(void) {
    uint8_t flags1, flags2;
    uint32_t close_at;

    if (pair_state == RADIO_PAIR_QUIESCE || !grid_started || device_count == 0u) {
        uplink_close();
        return;
    }

    /* While a pairing window is open the join region owns the tail of the
     * superframe, so the uplink receive has to end before it starts. */
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
        /* Tuned explicitly rather than inherited from the beacon. It used to
         * rely on "the beacon just went out on this hop channel", and the
         * downlink then transmitted at +25 ms through a helper that tuned the
         * join channel - so this window opened on 866.5 MHz for every
         * superframe carrying a downlink, which is half of them. Nothing
         * reported an error: the window opened, the counter rose, the band was
         * simply the wrong one.
         *
         * A receive window that inherits its channel is only correct until
         * something is scheduled before it. */
        uint8_t idx;

        if (hop_channel(&hop, frame_counter, &idx) != 0)
            return;
        if (rfm69_set_carrier_hz(&radio, slot_hz(hop_slot_to_grid(idx))) != RFM69_OK)
            return;
        if (rfm69_set_mode(&radio, RFM69_MODE_RX) != RFM69_OK)
            return;
        uplink_rx_open = 1;
        up_windows++;
        return;
    }

    if (rfm69_get_irq_flags(&radio, &flags1, &flags2) == RFM69_OK) {
        /* Counted separately from the join channel's sync. One counter for both
         * would rise on pairing traffic and read as uplink activity. */
        uint8_t was = sync_was_set;

        rx_note_sync(flags1);
        if (!was && sync_was_set)
            up_sync++;
        if (rx_frame_ready(flags2))
            handle_uplink_frame();
        /* After the frame and only with sync clear, for the same reason as the
         * join window: a triggered measurement overwrites the latch the packet
         * path reads, and that latch is the arriving frame's own level. */
        else if (!(flags1 & RFM69_IRQ1_SYNC_ADDR_MATCH)) {
            int16_t x2 = 0;

            if (rfm69_measure_rssi(&radio, RSSI_TIMEOUT_US, &x2) == RFM69_OK) {
                if (x2 > up_rssi_peak_x2)  up_rssi_peak_x2  = x2;
                if (x2 < up_rssi_floor_x2) up_rssi_floor_x2 = x2;
            }
        }
    }
}

static void handle_join_frame(void) {
    uint8_t len = 0;
    int16_t rssi_x2 = 0;

    if (rfm69_read_fifo(&radio, &len, 1) != RFM69_OK)
        return;
    if (len == 0u || len > sizeof(rx_buffer))
        return;
    if (rfm69_read_fifo(&radio, rx_buffer, len) != RFM69_OK)
        return;

    /* Recorded before a single check runs. Every counter below says why a frame
     * was refused; this says what it was, which is the question that actually
     * gets asked when two implementations disagree about a contract. */
    (void)rfm69_get_rssi(&radio, &rssi_x2);
    rx_last_len        = len;
    rx_last_type       = rx_buffer[0];
    rx_last_rssi       = (int8_t)(rssi_x2 / 2);
    rx_last_superframe = frame_counter;

    if (len < 2u || rx_buffer[1] != RADIO_PROTO_VERSION) {
        pair_reqs_dropped++;
        reqs_drop_last = RADIO_DROP_VERSION;
        return;
    }

    if (rx_buffer[0] == RADIO_FRAME_PAIR_CONF) {
        radio_pair_conf_t c;
        ipc_pair_conf_evt_t e;

        /* Only while this hub is actually waiting for one, and only from the
         * device the exchange belongs to. */
        if (ex_state != RADIO_EX_SENT_RSP || len < sizeof(c)) {
            pair_reqs_dropped++;
            reqs_drop_last = (len < sizeof(c)) ? RADIO_DROP_LEN : RADIO_DROP_BUSY;
            return;
        }
        memcpy(&c, rx_buffer, sizeof(c));
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

    if (rx_buffer[0] != RADIO_FRAME_PAIR_REQ) {
        pair_reqs_dropped++;
        reqs_drop_last = RADIO_DROP_TYPE;
        return;
    }

    {
        radio_pair_req_t req;
        ipc_pair_req_evt_t e;

        pair_reqs_seen++;
        if (len < sizeof(req)) {
            /* The device transmitted something calling itself a PAIR_REQ at a
             * length this side does not compile. That is a wire-contract
             * mismatch, not a radio problem, and it is the one drop reason that
             * says so unambiguously. */
            pair_reqs_dropped++;
            reqs_drop_last = RADIO_DROP_LEN;
            return;
        }
        /* Before anything judges it. This is the last point at which the
         * bytes are still the radio's rather than this core's reading. */
        memcpy(reqs_drop_head, rx_buffer, sizeof(reqs_drop_head));
        memcpy(reqs_drop_key,  rx_buffer + 24, sizeof(reqs_drop_key));
        memcpy(&req, rx_buffer, sizeof(req));

        /* Only the device the operator named may move this machine. A quiesce
         * is eight seconds of network silence, and the join channel is fixed
         * and published, so accepting any frame that arrives would hand a
         * denial of service to anyone in range. */
        /* A closed window is a working hub and an operator who has not pressed
         * the button. Counting it with a genuine identity mismatch makes a
         * non-fault look like one - and the same argument splits the rest: a
         * device the window was not opened for is an operator error, a wrong
         * hub_id is two hubs in range, a wrong net_id is a different network. */
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
        /* One exchange at a time. A second request while one is in flight is
         * dropped rather than allowed to replace it: the first device has
         * committed and is waiting on a response. */
        if (ex_state != RADIO_EX_IDLE && ex_state != RADIO_EX_ACCEPTED) {
            pair_reqs_dropped++;
            reqs_drop_last = RADIO_DROP_BUSY;
            return;
        }

        /* The exchange needs clear air before it needs arithmetic. Asking CM7
         * for 330 ms of curve work and only then discovering the rate limit
         * refuses the quiesce would spend it for nothing. */
        if (begin_quiesce(RADIO_QUIESCE_SUPERFRAMES) == 0) {
            quiesce_lost++;
            return;
        }

        e.dev_id     = req.dev_id;
        e.superframe = req.superframe;
        memcpy(e.dev_nonce, req.dev_nonce, sizeof(e.dev_nonce));
        memcpy(e.pubkey, req.pubkey, sizeof(e.pubkey));

        if (ipc_send_event(IPC_EVT_PAIR_REQ, &e, (uint8_t)sizeof(e), &ex_seq) != 0) {
            ex_reset();
            return;
        }
        ex_reqs_forwarded++;
        ex_dev_id   = req.dev_id;
        ex_waiting  = 1;
        ex_state    = RADIO_EX_WAIT_RSP;
        ex_retry    = 0;
        ex_deadline = rfm_micros() + timebase_us_to_ticks(EX_CM7_TIMEOUT_US);
    }
}

/* The join region overlays the tail of the uplink region and is only occupied
 * while a window is open, so it costs nothing the rest of the time. Slots are
 * assigned from 0 upward, which is what keeps it over unassigned slots.
 *
 * Split across superloop passes rather than blocking for the receive window:
 * 100 ms inside one iteration would sit across a superframe boundary and put
 * 100 ms of jitter into the beacon every device measures its period from. */
static void join_region_service(void) {
    uint8_t flags1, flags2;

    if (pair_state == RADIO_PAIR_IDLE) {
        if (join_phase) {
            (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
            join_phase = 0;
        }
        return;
    }

    if (join_phase == 0u) {
        /* With nothing paired, listen across the whole superframe instead of
         * for 100 ms in the join region.
         *
         * The 100 ms window asks a joining device to answer inside 5% of the
         * time, measured from the end of a beacon it has to have decoded. A
         * device that is 20 ms late is not late by a little - it waits 2 s for
         * another chance, and one that answers on its own schedule pairs about
         * one attempt in twenty, which on a bench reads as an intermittent
         * radio fault and stays alive as a wrong hypothesis for days.
         *
         * Receiving costs no duty cycle, so while there is no paired device to
         * serve there is nothing to trade against it. Gated on device_count so
         * a pairing window never costs an existing network its uplink - which
         * does leave the second device pairing under tighter timing than the
         * first, and that asymmetry is exactly what the hub-initiated redesign
         * removes rather than documents. */
        uint32_t open_tk = (pair_state == RADIO_PAIR_QUIESCE) ? 0u
                         : (device_count == 0u) ? timebase_us_to_ticks(RADIO_UPLINK_OFFSET_US)
                         : join_offset_tk;

        if (!grid_started || join_served_frame == frame_counter)
            return;
        if (!timebase_elapsed(superframe_start_tk + open_tk))
            return;
        join_served_frame = frame_counter;
        join_regions++;

        /* The uplink window ends exactly here - its close_at during LISTEN *is*
         * join_offset_tk - so the handoff is made explicit at the point the
         * radio changes owner rather than left to the order of two calls in the
         * superloop. uplink_service() runs after this one and its close would
         * otherwise drop the receiver to standby one line after the join region
         * armed it, on the same pass: retuned correctly, beacon transmitted,
         * counters all rising, and deaf.
         *
         * Only reachable with a device installed, because the uplink window
         * does not exist until then - so the first pairing worked and every
         * later one could not. */
        uplink_close();

        /* Half rate on the join channel keeps the extra air time to 0.21%, and
         * a joiner still finds the hub inside two superframes. */
        /* The beacon keeps its documented 1874 ms offset even when the receive
         * window opens early: devices measure their reply from it, and it has
         * been verified on air at that position. Only the listening moved. */
        if (device_count == 0u && pair_state == RADIO_PAIR_LISTEN) {
            if (rfm69_set_carrier_hz(&radio, slot_hz(RADIO_JOIN_SLOT)) != RFM69_OK)
                return;
            join_beacon_pending = ((frame_counter % JOIN_BEACON_EVERY) == 0u);
        } else if ((frame_counter % JOIN_BEACON_EVERY) == 0u &&
                   pi_superframe != frame_counter) {
            RFM_send_join_beacon();
        } else if (rfm69_set_carrier_hz(&radio, slot_hz(RADIO_JOIN_SLOT)) != RFM69_OK) {
            return;
        }

        if (rfm69_set_mode(&radio, RFM69_MODE_RX) != RFM69_OK)
            return;
        /* Listening is what a quiesce is for, so during one the window runs to
         * the boundary instead of closing after the join region. It still stops
         * short of it: the next beacon must not find a receive in progress. */
        join_rx_deadline = (pair_state == RADIO_PAIR_QUIESCE || device_count == 0u)
            ? superframe_start_tk + superframe_tk
              - timebase_us_to_ticks(RADIO_END_GUARD_US)
            : rfm_micros() + timebase_us_to_ticks(RADIO_JOIN_RX_US);
        join_phase = 1;
        return;
    }

    if (join_beacon_pending &&
        timebase_elapsed(superframe_start_tk + join_offset_tk)) {
        join_beacon_pending = 0;
        /* Out of RX first. Writing the FIFO while the receiver is running mixes
         * the outgoing frame with whatever the packet engine has collected, and
         * the transmit fails - measured as tx err 2 for 2 beacons on the first
         * build of this path. */
        (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
        RFM_send_join_beacon();
        (void)rfm69_set_mode(&radio, RFM69_MODE_RX);   /* straight back to listening */
    }

    if (rfm69_get_irq_flags(&radio, &flags1, &flags2) == RFM69_OK) {
        rx_note_sync(flags1);
        if (rx_frame_ready(flags2))
            handle_join_frame();
        /* After the frame, never before it. A triggered measurement overwrites
         * the latch, and the latch is the arriving packet's own level - the one
         * number with no second chance at it. */
        else if (!(flags1 & RFM69_IRQ1_SYNC_ADDR_MATCH))
            rx_sample_rssi();
    }

    if (timebase_elapsed(join_rx_deadline)) {
        (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
        sync_was_set = 0;
        join_beacon_pending = 0;
        join_phase = 0;
    }
}

/* 1 when this call armed a quiesce. Reporting "already quiescing" as success
 * would make a caller count arming attempts as quiesces, which is how a test
 * ends up comparing 8 announcements against 4 that happened. */
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

    if (superframe_due())
        on_superframe();

    join_region_service();
    pair_init_service();
    exchange_service();
    downlink_service();
    uplink_service();

    /* Only in the first half of the superframe. A flash program stalls this
     * core for the best part of a millisecond, and the beacon's offset - 1 to
     * 4 us, the number every device's period estimate rests on - must not
     * inherit it. There are 4096 superframes of chances to take. */
    if (grid_started && !timebase_elapsed(superframe_start_tk + superframe_tk / 2u))
        (void)kv_reserve(frame_counter);

    /* Drained by polling rather than off the HSEM flag: a missed doorbell then
     * costs latency instead of a lost request. The flag is still cleared so it
     * does not stay pending. */
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M7_TO_M4_RFM));
    while (ipc_poll_request(&request))
        RFM_serve_request(&request);
}

/*  @retval 0 - window opened
 *
 *  Opening a window instead of running the exchange inline: the old routine sat
 *  in this loop for ten seconds, which a slot grid cannot afford. The join
 *  beacon goes out from RFM_Routine like any other frame, and the window closes
 *  on its own deadline. */
/* Every counter below the frame layer is cumulative since boot, and a running
 * maximum from an unknown superframe answers no question anyone asks of it:
 * "did that transmission arrive" needs a sweep, not a total. The window is the
 * natural sweep boundary - it is the only interval during which the receiver
 * is on at all - so opening one starts a clean measurement. */
static void rx_diag_reset(void) {
    rx_sync_match = 0;
    rx_frames = 0;
    rx_crc_err = 0;
    rx_rssi_peak_x2 = -32768;
    rx_rssi_floor_x2 = 32767;
    rx_rssi_samples = 0;
    rx_last_len = 0;
    rx_last_type = 0;
    rx_last_rssi = 0;
    rx_last_superframe = 0;
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
