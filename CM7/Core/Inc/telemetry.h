#pragma once

#include <stdint.h>

#include "ipc.h"

/**
 * @file telemetry.h
 * @brief The northbound link: telemetry out, commands in, over one TCP socket.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */

#define TELEMETRY_TOKEN_MAX  32u

/**
 * @brief The link's own health, counted so a gap in the data names its cause.
 *
 * A dashboard that stops updating has three explanations and one symptom, so
 * every one of them is a separate counter here rather than a shared total.
 * radio_devices_docs/open_hub/network/telemetry.md
 */
typedef struct telemetry_stats {
    uint32_t connects;
    uint32_t disconnects;
    uint32_t connect_fail;
    uint8_t  last_disc_reason;  /**< OHT_DISC_REASON_* */
    uint8_t  connected;
    uint8_t  enabled;
    uint8_t  hello_reason;      /**< OHT_ACK_* from the last refusal */
    uint32_t frames_tx;
    uint32_t bytes_tx;
    uint32_t tx_fail;
    uint32_t cmds_rx;
    uint32_t cmds_bad;
    uint32_t events_pushed;
    uint32_t events_dropped;    /**< arrivals the queue could not hold */
    uint32_t snapshots;
    uint32_t snapshot_us;       /**< what the last one cost, IPC round trips included */
    uint32_t snapshot_trunc;    /**< bodies the buffer refused a record of */
    uint32_t up_ms;             /**< ticks the current session has lasted */
    uint32_t snapshot_ms;       /**< the cadence the server asked for */
    uint32_t snapshot_gap_ms;   /**< ... and the one actually achieved */
} telemetry_stats_t;

/**
 * @brief The task: connects, keeps the link, and answers whatever arrives on it.
 * @param argument  unused, required by the CMSIS-RTOS signature
 */
void TelemetryTask(void *argument);

/**
 * @brief Points the link at a server. Takes effect on the next connect attempt.
 * @param ip     the server's address, as text
 * @param port   its port
 * @param token  the shared secret, or NULL to send none
 * @retval 0  accepted
 * @retval -1 the address did not parse
 *
 * Not persisted: `cfg save` is still a stub, so a reset forgets this.
 */
int telemetry_configure(const char *ip, uint16_t port, const char *token);

/**
 * @brief Opens or closes the link.
 * @param on  non-zero to connect and stay connected
 */
void telemetry_enable(uint8_t on);

/** @brief Asks for a snapshot on the next pass rather than at the next tick. */
void telemetry_request_snapshot(void);

/**
 * @brief The link's counters and where it is pointed.
 * @param ip    receives the configured address as text, or NULL
 * @param port  receives the port, or NULL
 * @return the live block
 */
const telemetry_stats_t *telemetry_get_stats(const char **ip, uint16_t *port);

/**
 * @brief Hands one uplink arrival to the link, from whichever task saw it.
 * @param r  the report CM4 pushed
 *
 * Queued rather than sent: this runs in pairTask, and a socket write there
 * would put the pairing state machine behind the network.
 * radio_devices_docs/open_hub/network/telemetry.md
 */
void telemetry_notify_uplink(const ipc_device_report_t *r);
