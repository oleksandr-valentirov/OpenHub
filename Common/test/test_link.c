/**
 * @file test_link.c
 * @brief The host consumer the link vectors never had: layout, not cryptography.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */

#include <stdio.h>
#include <string.h>
#include "radio_protocol.h"
#include "link_v5.h"

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* The generator's values, laid out by C rather than by struct.pack. */
#define V_RSSI_DOWN    (-92)
#define V_FLAGS        (RADIO_REPORT_FLAG_RSSI_STALE | RADIO_REPORT_FLAG_RESUMED)
#define V_SUPPLY_MV    3287u
#define V_UPTIME_S     61u
#define V_ACK_SEQ      0x5Bu
#define V_ACK_ARG      31u
#define V_REPORT_EVERY 12u
#define V_ARG          0x1234u
#define V_HUB_TIME_S   0x00112233u
#define V_CMD_SEQ      0x5Bu
#define V_UP_SLOT      66u
#define V_UP_SF        0x1a2b3c58u
#define V_DL_SLOT      1u
#define V_DL_SF        0x1a2b3c59u

static void diff(const char *what, const uint8_t *got, const uint8_t *want, size_t n) {
    size_t i, first = 0;

    if (memcmp(got, want, n) == 0)
        return;
    while (first < n && got[first] == want[first])
        first++;
    printf("FAIL %s\n  built    ", what);
    for (i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n  vector   ");
    for (i = 0; i < n; i++) printf("%02x", want[i]);
    /* The offset names the field, which a mismatch report otherwise does not. */
    printf("\n  first differing byte %u\n", (unsigned)first);
    fails++;
}

int main(void) {
    radio_uplink_report_t rpt;
    radio_downlink_cmd_t  cmd;
    radio_uplink_t        up;
    radio_downlink_t      dl;
    static const uint8_t app_up[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };
    static const uint8_t app_dl[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    /* A size assert answers "same shape", never "same contract". */
    CHECK(LINK_VECTORS_VERSION == RADIO_LINK_VERSION);

    memset(&rpt, 0, sizeof(rpt));
    rpt.rssi_down = V_RSSI_DOWN;
    rpt.flags     = V_FLAGS;
    rpt.supply_mv = V_SUPPLY_MV;
    rpt.uptime_s  = V_UPTIME_S;
    rpt.ack_seq   = V_ACK_SEQ;
    rpt.ack_cmd   = RADIO_CMD_SET_RATE;
    rpt.ack_arg   = V_ACK_ARG;
    rpt.app_len   = (uint8_t)sizeof(app_up);
    memcpy(rpt.app, app_up, sizeof(app_up));
    CHECK(sizeof(rpt) == sizeof(LV_UPLINK_PLAIN));
    diff("uplink report layout", (const uint8_t *)&rpt, LV_UPLINK_PLAIN, sizeof(rpt));

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd          = RADIO_CMD_SET_RATE;
    cmd.report_every = V_REPORT_EVERY;
    cmd.arg          = V_ARG;
    cmd.hub_time_s   = V_HUB_TIME_S;
    cmd.cmd_seq      = V_CMD_SEQ;
    cmd.app_len      = (uint8_t)sizeof(app_dl);
    memcpy(cmd.app, app_dl, sizeof(app_dl));
    CHECK(sizeof(cmd) == sizeof(LV_DOWNLINK_PLAIN));
    diff("downlink command layout", (const uint8_t *)&cmd, LV_DOWNLINK_PLAIN, sizeof(cmd));

    /* The AAD is the cleartext header, so this pins what the tag commits to. */
    memset(&up, 0, sizeof(up));
    up.type       = RADIO_FRAME_UPLINK;
    up.version    = RADIO_LINK_VERSION;
    up.slot       = V_UP_SLOT;
    up.superframe = V_UP_SF;
    diff("uplink AAD layout", (const uint8_t *)&up, LV_UPLINK_AAD, sizeof(LV_UPLINK_AAD));

    memset(&dl, 0, sizeof(dl));
    dl.type       = RADIO_FRAME_DOWNLINK;
    dl.version    = RADIO_LINK_VERSION;
    dl.slot       = V_DL_SLOT;
    dl.superframe = V_DL_SF;
    diff("downlink AAD layout", (const uint8_t *)&dl, LV_DOWNLINK_AAD, sizeof(LV_DOWNLINK_AAD));

    /* The published frame must open with its own AAD, or the two disagree. */
    diff("uplink frame header", LV_FRAME_UPLINK, LV_UPLINK_AAD, sizeof(LV_UPLINK_AAD));
    diff("downlink frame header", LV_FRAME_DOWNLINK, LV_DOWNLINK_AAD, sizeof(LV_DOWNLINK_AAD));

    if (fails == 0)
        printf("link: ok (v%u, layout of both sealed bodies and both AADs)\n",
               (unsigned)LINK_VECTORS_VERSION);
    return fails != 0;
}
