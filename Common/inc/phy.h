#pragma once

#include <stdint.h>

#include "radio_phy.h"

/**
 * @file phy.h
 * @brief The radio logic's whole view of a chip: nine operations and one event.
 *
 * **One contract, two firmwares, no copy.** It lives here because both trees
 * already build against this directory - the device through OPENHUB_PATH - and
 * because a contract each side owns a copy of is a contract one side can revise
 * alone. That is the hazard ADR-0028 was written after.
 *
 * The device has an SX126x under this and the hub an RFM69. Nothing above the
 * nine operations learns which, and that is the point: identical logic on two
 * chips separates *the logic is wrong* from *this driver is wrong*, which no
 * counter on either side of the antenna can do.
 *
 * This file carries no HAL, no chip register and no store, and it must not grow
 * one - wl55_device/ROADMAP.md item 76.
 *
 * radio_devices_docs/radio/phy-seam.md
 * radio_devices_docs/radio/decisions/0028-the-radio-is-a-library-and-the-region-is-a-compile-time-profile.md
 */

/* Every frame the protocol defines fits; the ceiling is the hub FIFO's, not this file's. */
#define PHY_MAX_PAYLOAD  RADIO_MAX_PAYLOAD_B

/* Not 0: on an RFM69 that is G1, a gain the AGC really does choose. */
#define PHY_LNA_UNKNOWN  0xFFu

/** @brief What one poll found. A failed CRC is an event, never a silence. */
typedef enum {
    PHY_EV_NONE = 0,   /**< nothing since the last poll */
    PHY_EV_SYNC,       /**< a sync word matched and the payload is still arriving */
    PHY_EV_FRAME,      /**< a whole frame, CRC verified */
    PHY_EV_CRC         /**< a whole frame arrived and its CRC failed */
} phy_ev_kind_t;

typedef struct {
    phy_ev_kind_t kind;
    uint8_t  len;                    /**< payload bytes in buf, 0 unless kind is FRAME */
    uint8_t  buf[PHY_MAX_PAYLOAD];
    int16_t  rssi_dbm;               /**< this frame's own level, 0 when none was read */
    uint32_t sync_us;                /**< the sync edge on phy_now_us's clock */
    uint8_t  sync_valid;             /**< 0 when no edge was timestamped for this frame */
    uint32_t sync_seq;               /**< which edge sync_us came from; only ever rises */
    uint8_t  busy;                   /**< a frame is arriving: do not survey the band now */
    int32_t  afc_hz;                 /**< carrier error this frame arrived with */
    uint8_t  afc_valid;              /**< 0 when the part could not report it */
    uint8_t  lna_gain;               /**< front-end gain index at this frame, 0xFF unknown */
} phy_ev_t;

/* A backend zeroes the event first, so nothing found has to be what zero means. */
_Static_assert(PHY_EV_NONE == 0, "a zeroed event must read as nothing found");
/* The trap this constant exists for, held in both firmwares rather than in a test. */
_Static_assert(PHY_LNA_UNKNOWN != 0, "unknown must not be a gain a part can report");
_Static_assert(PHY_MAX_PAYLOAD >= RADIO_MAX_PAYLOAD_B,
               "a frame the protocol defines does not fit the event");

/** @brief Brings the chip up on the protocol's PHY. */
int phy_init(void);

/** @brief Tunes the carrier; the caller names the channel, never this layer. */
int phy_tune(uint32_t hz);

/** @brief Enters continuous receive and stays there across frames and CRC failures. */
int phy_listen(void);

/** @brief Leaves receive without changing the configuration. */
int phy_standby(void);

/** @brief Transmits, blocking to the end of the frame, and returns to receive. */
int phy_transmit(const void *payload, uint8_t len, uint32_t *air_us);

/** @brief Non-blocking; fills ev with what the chip has, PHY_EV_NONE when nothing. */
int phy_poll(phy_ev_t *ev);

/** @brief The level with no frame on it, which is the floor the frames stand on. */
int16_t phy_rssi_now(void);

/** @brief One clock for every timestamp this interface reports, in microseconds. */
uint32_t phy_now_us(void);
