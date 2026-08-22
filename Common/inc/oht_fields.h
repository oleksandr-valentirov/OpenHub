#pragma once

/**
 * @file oht_fields.h
 * @brief Generated from openhub-server/schema/telemetry.yaml. Do not edit by hand.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */

/* The digest the hub sends in HELLO; the server compares it with its own. */
#define OHT_SCHEMA_DIGEST  "9f5b89b1994488c3"
#define OHT_SCHEMA_VERSION 1u

/* Object codes: which table a record's fields are read out of. */
#define OHT_OBJ_HUB      1u
#define OHT_OBJ_DEVICE   2u
#define OHT_OBJ_RXDIAG   3u
#define OHT_OBJ_LINK     4u
#define OHT_OBJ_FRAME    5u

/* hub: The hub itself - grid, timing, and the counters that are not per device.
 * radio_devices_docs/open_hub/network/telemetry.md */
#define OHT_F_HUB_UPTIME_MS          0x0100u, OHT_T_U32
#define OHT_F_HUB_SUPERFRAME         0x0101u, OHT_T_U32
#define OHT_F_HUB_PERIOD_US          0x0102u, OHT_T_U32
#define OHT_F_HUB_CALIB_PPM          0x0103u, OHT_T_I32
#define OHT_F_HUB_LATE_LAST_US       0x0104u, OHT_T_U32
#define OHT_F_HUB_LATE_MAX_US        0x0105u, OHT_T_U32
#define OHT_F_HUB_LATE_OVER          0x0106u, OHT_T_U32
#define OHT_F_HUB_CALIB_WINDOWS      0x0107u, OHT_T_U32
#define OHT_F_HUB_CALIB_REJECTS      0x0108u, OHT_T_U32
#define OHT_F_HUB_DEVICES            0x0120u, OHT_T_U8
#define OHT_F_HUB_PAIR_STATE         0x0121u, OHT_T_U8
#define OHT_F_HUB_PAIRED_TOTAL       0x0122u, OHT_T_U32
#define OHT_F_HUB_REPORT_EVERY       0x0123u, OHT_T_U8
#define OHT_F_HUB_AEAD_SELFTEST      0x0124u, OHT_T_U8
#define OHT_F_HUB_UPLINK_FRAMES      0x0130u, OHT_T_U32
#define OHT_F_HUB_UPLINK_OK          0x0131u, OHT_T_U32
#define OHT_F_HUB_UPLINK_BAD_TAG     0x0132u, OHT_T_U16
#define OHT_F_HUB_UPLINK_BAD_SLOT    0x0133u, OHT_T_U16
#define OHT_F_HUB_UPLINK_BAD_FRAME   0x0134u, OHT_T_U16
#define OHT_F_HUB_UPLINK_REPLAY      0x0135u, OHT_T_U16
#define OHT_F_HUB_UPLINK_WINDOWS     0x0136u, OHT_T_U32
#define OHT_F_HUB_UPLINK_SYNC        0x0137u, OHT_T_U32
#define OHT_F_HUB_UPLINK_EVT_DROP    0x0138u, OHT_T_U32
#define OHT_F_HUB_DL_OPPORTUNITIES   0x0140u, OHT_T_U32
#define OHT_F_HUB_DL_SENT            0x0141u, OHT_T_U32
#define OHT_F_HUB_DL_TX_ERR          0x0142u, OHT_T_U32
#define OHT_F_HUB_DL_CMD_SENT        0x0143u, OHT_T_U32
#define OHT_F_HUB_DL_CMD_ACKED       0x0144u, OHT_T_U32
#define OHT_F_HUB_DL_CMD_LOST        0x0145u, OHT_T_U32
#define OHT_F_HUB_DL_NONCE_REFUSED   0x0146u, OHT_T_U32
#define OHT_F_HUB_IPC_STALE_REPLIES  0x0150u, OHT_T_U32
#define OHT_F_HUB_IPC_STALE_EVENTS   0x0151u, OHT_T_U32
#define OHT_F_HUB_EVT_RTT_LAST_US    0x0152u, OHT_T_U32
#define OHT_F_HUB_EVT_RTT_MAX_US     0x0153u, OHT_T_U32
#define OHT_F_HUB_EVT_LOST           0x0154u, OHT_T_U32
#define OHT_F_HUB_IPC_READY          0x0155u, OHT_T_BOOL

/* device: One paired device, keyed by its 32-bit identifier.
 * radio_devices_docs/open_hub/network/telemetry.md */
#define OHT_F_DEVICE_SLOT               0x1000u, OHT_T_U8
#define OHT_F_DEVICE_LAST_SUPERFRAME    0x1001u, OHT_T_U32
#define OHT_F_DEVICE_FRAMES_OK          0x1002u, OHT_T_U32
#define OHT_F_DEVICE_FRAMES_BAD         0x1003u, OHT_T_U32
#define OHT_F_DEVICE_DEV_UPTIME_S       0x1004u, OHT_T_U32
#define OHT_F_DEVICE_SUPPLY_MV          0x1005u, OHT_T_U16
#define OHT_F_DEVICE_REPORT_EVERY       0x1006u, OHT_T_U8
#define OHT_F_DEVICE_REPORT_FLAGS       0x1007u, OHT_T_U8
#define OHT_F_DEVICE_ARRIVAL_US         0x1008u, OHT_T_U32
#define OHT_F_DEVICE_RSSI_UP_SYNC_DBM   0x1010u, OHT_T_I8
#define OHT_F_DEVICE_RSSI_UP_LATCH_DBM  0x1011u, OHT_T_I8
#define OHT_F_DEVICE_RSSI_DOWN_DBM      0x1012u, OHT_T_I8
#define OHT_F_DEVICE_LNA_GAIN           0x1013u, OHT_T_U8
#define OHT_F_DEVICE_AFC_HZ             0x1014u, OHT_T_I32
#define OHT_F_DEVICE_CYC_MIN_MS         0x1020u, OHT_T_U16
#define OHT_F_DEVICE_CYC_N              0x1021u, OHT_T_U16
#define OHT_F_DEVICE_CYC_SUM_MS         0x1022u, OHT_T_U32
#define OHT_F_DEVICE_CMD_STATE          0x1030u, OHT_T_U8
#define OHT_F_DEVICE_CMD_EVERY          0x1031u, OHT_T_U8
#define OHT_F_DEVICE_ACK_ARG            0x1032u, OHT_T_U8

/* rxdiag: What the receiver hears below the frame layer, hub-wide.
 * radio_devices_docs/open_hub/network/telemetry.md */
#define OHT_F_RXDIAG_SYNC_MATCH           0x2000u, OHT_T_U32
#define OHT_F_RXDIAG_CRC_ERR              0x2001u, OHT_T_U32
#define OHT_F_RXDIAG_FRAMES               0x2002u, OHT_T_U32
#define OHT_F_RXDIAG_FLUSHES              0x2003u, OHT_T_U32
#define OHT_F_RXDIAG_RSSI_SAMPLES         0x2004u, OHT_T_U32
#define OHT_F_RXDIAG_UP_RSSI_PEAK_DBM     0x2005u, OHT_T_I8
#define OHT_F_RXDIAG_UP_RSSI_FLOOR_DBM    0x2006u, OHT_T_I8
#define OHT_F_RXDIAG_JOIN_RSSI_PEAK_DBM   0x2007u, OHT_T_I8
#define OHT_F_RXDIAG_JOIN_RSSI_FLOOR_DBM  0x2008u, OHT_T_I8
#define OHT_F_RXDIAG_LAST_RSSI_DBM        0x2009u, OHT_T_I8

/* link: The telemetry link's own health, so a gap in the data names its cause.
 * radio_devices_docs/open_hub/network/telemetry.md */
#define OHT_F_LINK_CONNECTS          0x3000u, OHT_T_U32
#define OHT_F_LINK_DISCONNECTS       0x3001u, OHT_T_U32
#define OHT_F_LINK_LAST_DISC_REASON  0x3002u, OHT_T_U8
#define OHT_F_LINK_FRAMES_TX         0x3003u, OHT_T_U32
#define OHT_F_LINK_BYTES_TX          0x3004u, OHT_T_U32
#define OHT_F_LINK_TX_FAIL           0x3005u, OHT_T_U32
#define OHT_F_LINK_CMDS_RX           0x3006u, OHT_T_U32
#define OHT_F_LINK_CMDS_BAD          0x3007u, OHT_T_U32
#define OHT_F_LINK_EVENTS_PUSHED     0x3008u, OHT_T_U32
#define OHT_F_LINK_EVENTS_DROPPED    0x3009u, OHT_T_U32
#define OHT_F_LINK_SNAPSHOT_US       0x300au, OHT_T_U32
#define OHT_F_LINK_SNAPSHOT_ASK_MS   0x300bu, OHT_T_U32
#define OHT_F_LINK_SNAPSHOT_GAP_MS   0x300cu, OHT_T_U32

/* frame: One received frame from the AFC ring - the per-frame scatter, not a mean.
 * radio_devices_docs/open_hub/network/telemetry.md */
#define OHT_F_FRAME_SEQ       0x4000u, OHT_T_U32
#define OHT_F_FRAME_GRID      0x4001u, OHT_T_U8
#define OHT_F_FRAME_SLOT      0x4002u, OHT_T_U8
#define OHT_F_FRAME_RSSI_DBM  0x4003u, OHT_T_I8
#define OHT_F_FRAME_LNA_GAIN  0x4004u, OHT_T_U8
#define OHT_F_FRAME_AFC_HZ    0x4005u, OHT_T_I32
#define OHT_F_FRAME_CRC_OK    0x4006u, OHT_T_BOOL
#define OHT_F_FRAME_IN_FRAME  0x4007u, OHT_T_BOOL

/* Command argument ids: a separate space from the fields above. */
#define OHT_A_DEV_ID     0x8000u, OHT_T_U32
#define OHT_A_RATE       0x8001u, OHT_T_U8
#define OHT_A_WINDOW_MS  0x8002u, OHT_T_U32
#define OHT_A_PUBKEY     0x8003u, OHT_T_BYTES
#define OHT_A_GAIN       0x8004u, OHT_T_U8
#define OHT_A_REPEATS    0x8005u, OHT_T_U8
#define OHT_A_APP        0x8006u, OHT_T_BYTES
#define OHT_A_LEVEL      0x8007u, OHT_T_U8

/** @brief What the server may ask the hub to do; `scope` decides `target`. */
enum {
    OHT_CMD_PING               = 0x0001u,
    OHT_CMD_SNAPSHOT_NOW       = 0x0002u,
    OHT_CMD_SET_REPORT_RATE    = 0x0003u,
    OHT_CMD_PAIR_WINDOW        = 0x0004u,
    OHT_CMD_DEVICE_ADD         = 0x0005u,
    OHT_CMD_DEVICE_REMOVE      = 0x0006u,
    OHT_CMD_SET_LNA            = 0x0007u,
    OHT_CMD_DEV_SET_RATE       = 0x1001u,
    OHT_CMD_DEV_REJOIN         = 0x1002u,
    OHT_CMD_DEV_NOP            = 0x1003u,
    OHT_CMD_DEV_APP            = 0x1004u,   /**< transport only: the radio end is unagreed */
};

/** @brief A CMD_ACK's verdict. `queued` is not `ok`: nothing has flown yet. */
enum {
    OHT_RES_OK                 = 0u,
    OHT_RES_QUEUED             = 1u,
    OHT_RES_UNKNOWN_CMD        = 2u,
    OHT_RES_BAD_ARGS           = 3u,
    OHT_RES_NO_SUCH_DEVICE     = 4u,
    OHT_RES_BUSY               = 5u,
    OHT_RES_RADIO_ERR          = 6u,
    OHT_RES_NOT_PERMITTED      = 7u,
    OHT_RES_NOT_IMPLEMENTED    = 8u,
};

/* cmd_state, as the schema names it; the wire carries the number. */
#define OHT_CMD_STATE_NONE 0u
#define OHT_CMD_STATE_RIDING_DOWNLINKS 1u
#define OHT_CMD_STATE_ACKED 2u

/* disc_reason, as the schema names it; the wire carries the number. */
#define OHT_DISC_REASON_NONE 0u
#define OHT_DISC_REASON_CONNECT_FAILED 1u
#define OHT_DISC_REASON_PEER_CLOSED 2u
#define OHT_DISC_REASON_SEND_FAILED 3u
#define OHT_DISC_REASON_RECV_FAILED 4u
#define OHT_DISC_REASON_BAD_FRAME 5u
#define OHT_DISC_REASON_HELLO_REFUSED 6u
#define OHT_DISC_REASON_KEEPALIVE_LOST 7u
#define OHT_DISC_REASON_OPERATOR 8u

/* lna_gain, as the schema names it; the wire carries the number. */
#define OHT_LNA_GAIN_UNSET 0u
#define OHT_LNA_GAIN_G1_MAX 1u
#define OHT_LNA_GAIN_G2_ATT6 2u
#define OHT_LNA_GAIN_G3_ATT12 3u
#define OHT_LNA_GAIN_G4_ATT24 4u
#define OHT_LNA_GAIN_G5_ATT36 5u
#define OHT_LNA_GAIN_G6_ATT48 6u
#define OHT_LNA_GAIN_READ_FAILED 255u

/* pair_state, as the schema names it; the wire carries the number. */
#define OHT_PAIR_STATE_IDLE 0u
#define OHT_PAIR_STATE_LISTEN 1u
#define OHT_PAIR_STATE_QUIESCE 2u

