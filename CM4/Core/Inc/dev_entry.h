#pragma once

#include <stdint.h>

#include "aead.h"

/**
 * @file dev_entry.h
 * @brief One device as CM4 holds it, in RAM, for the life of one boot.
 *
 * radio_devices_docs/open_hub/radio/superloop.md
 */
typedef struct dev_entry {
    uint8_t  used;
    uint8_t  slot;
    uint8_t  report_every;      /**< granted at pairing, and never rewritten after it */
    uint8_t  every_now;         /**< the period in force: the grant until a SET_RATE is acked */
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
    int8_t   rssi_up;           /**< dBm at the end of the last accepted frame */
    int8_t   rssi_up_sync;      /**< dBm at the sync edge of that same frame */
    int32_t  afc_hz;            /**< ... and its carrier error */
    uint8_t  lna_gain;          /**< ... and the gain in force on it */
    uint8_t  air_have;          /**< IPC_AIR_*: which of the four were measured */
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
    uint8_t  dl_app_len;        /**< APP only; bytes of dl_app the queued command carries */
    uint8_t  dl_app[6];         /**< ... and those bytes, held per device like the rest of it */
    uint32_t dl_nonce_sf;       /**< the superframe of the last downlink sealed for it */
    uint8_t  dl_nonce_used;     /**< ... and whether there was one, since 0 is a real one */
    uint16_t missed_run;        /**< report opportunities closed in a row with nothing */
    uint32_t cyc_last_sf;       /**< superframe of the last cycle that arrived */
    uint16_t cyc_min;           /**< its shortest gap: what the device's cadence is */
    uint16_t cyc_n;
    uint32_t cyc_sum;           /**< ... against the mean, which also carries loss */
    uint16_t up_seq;            /**< the device's own count of what it attempted */
} dev_entry_t;
