/**
 * @file phy_rfm69.c
 * @brief The RFM69 behind the eight calls of phy.h, and the board under it.
 *
 * Twenty of the driver's twenty-nine calls are configuration and every one runs
 * exactly once, from values that already live in radio_phy.h. They are not the
 * protocol's business at all, so they live here and the layer above never learns
 * the chip has registers.
 *
 * The device has an SX126x under the same eight. That is what makes a
 * second PHY a control rather than a port: identical logic on a chip that is not
 * this one separates *the logic is wrong* from *this driver is wrong*, which no
 * counter on either side of the antenna can do.
 *
 * radio_devices_docs/radio/phy-seam.md
 * OpenHub/ROADMAP.md item 75
 */
#include <stddef.h>
#include <string.h>

#include "phy.h"
#include "phy_rfm69.h"
#include "rfm69_registers.h"
#include "radio_phy.h"
#include "clock.h"
#include "main.h"

/* The driver gives up on a mode change after this, rather than spinning. */
#define MODE_TIMEOUT_US   10000u
/* PacketSent has to arrive inside this or the frame is called lost. */
#define TX_TIMEOUT_US     200000u
/* A triggered RSSI measurement that does not complete is not a quiet band. */
#define RSSI_TIMEOUT_US   500u

/* Off: this schedule cannot meet AFC's precondition. ADR-0033
 * radio_devices_docs/open_hub/decisions/0033-the-hub-does-not-run-afc.md */
#ifndef RADIO_AFC_AUTO
#define RADIO_AFC_AUTO 0
#endif
/* Set but inert: the packet filter is FILTER_NONE. */
#define BROADCAST_ADDR    255

static rfm69_dev_t radio;
static uint8_t     dio_map1;
/* The length byte and the payload, which is what this part puts on the air. */
static uint8_t     tx_buffer[RFM69_FIFO_SIZE];
/* Where a frame that failed its CRC is read out to, and read out it must be. */
static uint8_t     drain_buffer[PHY_MAX_PAYLOAD];

/* SyncAddressMatch is a level, not a pulse, so a frame is the rise of this shadow.
 * radio_devices_docs/open_hub/radio/configuration.md */
static uint8_t sync_was_set;
/* Stamped on the DIO3 edge, which is earlier and steadier than any poll.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static volatile uint32_t sync_edge_tk;
static volatile uint32_t sync_edge_seq;

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

rfm69_dev_t *phy_rfm69_dev(void) { return &radio; }

uint8_t phy_rfm69_dio_map1(void) { return dio_map1; }

/* --- the contract --- */

/* No carrier here: phy.h says the caller names the channel.
 * radio_devices_docs/radio/phy-seam.md */
int phy_init(void) {
    static const uint8_t sync_val[] = {'h', 'e', 'l', 'l'};

    if (rfm69_init(&radio, &radio_io) != RFM69_OK)
        return -1;
    if (rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US) != RFM69_OK)
        return -1;

    if (rfm69_set_bitrate(&radio, RADIO_BITRATE_BPS) != RFM69_OK) return -1;
    if (rfm69_set_deviation_hz(&radio, RADIO_DEVIATION_HZ) != RFM69_OK) return -1;
    if (rfm69_set_rx_bandwidth_hz(&radio, RADIO_RX_BANDWIDTH_HZ) != RFM69_OK) return -1;
    /* Named explicitly; the reset values put the threshold below the noise floor.
     * radio_devices_docs/open_hub/radio/configuration.md */
    if (rfm69_set_rssi_threshold_dbm(&radio, -100) != RFM69_OK) return -1;
    if (rfm69_set_dagc(&radio, 0) != RFM69_OK) return -1;
    /* RegAfcValue stays 0 while off, so the ring's afc column checks the setting.
     * radio_devices_docs/open_hub/radio/configuration.md */
    if (rfm69_set_afc(&radio, RADIO_AFC_AUTO) != RFM69_OK) return -1;
    /* DIO3 = SyncAddressMatch, then RegDioMapping1 read back off the part. */
    if (rfm69_set_dio(&radio, 3, RFM69_DIO3_SYNC_ADDRESS) != RFM69_OK) return -1;
    if (rfm69_read_reg(&radio, RFM69_RegDioMapping1, &dio_map1) != RFM69_OK)
        return -1;
    if (rfm69_set_modulation(&radio, RFM69_SHAPING_BT_0_5) != RFM69_OK) return -1;
    if (rfm69_set_preamble_bytes(&radio, RADIO_PREAMBLE_BYTES) != RFM69_OK)
        return -1;
    if (rfm69_set_sync(&radio, sync_val, sizeof(sync_val), 0) != RFM69_OK) return -1;

    /* Neither Manchester nor whitening.
     * radio_devices_docs/open_hub/radio/configuration.md */
    if (rfm69_set_packet_format(&radio, 1, RFM69_DCFREE_NONE, 1,
                                RFM69_FILTER_NONE) != RFM69_OK) return -1;
    /* CrcAutoClearOff: a frame failing CRC is delivered rather than discarded.
     * radio_devices_docs/open_hub/radio/configuration.md */
    {
        uint8_t pc1 = 0;

        if (rfm69_read_reg(&radio, RFM69_RegPacketConfig1, &pc1) != RFM69_OK)
            return -1;
        if (rfm69_write_reg(&radio, RFM69_RegPacketConfig1,
                            (uint8_t)(pc1 | 0x08u)) != RFM69_OK)
            return -1;
    }
    if (rfm69_set_payload_length(&radio, RFM69_FIFO_SIZE - 2) != RFM69_OK) return -1;
    /* Inert: filtering is FILTER_NONE above.
     * radio_devices_docs/open_hub/radio/configuration.md */
    if (rfm69_set_broadcast_address(&radio, BROADCAST_ADDR) != RFM69_OK) return -1;
    /* PA1: this module's PA0 pin is not bonded to the antenna.
     * radio_devices_docs/open_hub/radio/configuration.md */
    if (rfm69_set_power(&radio, RFM69_PA1, 13) != RFM69_OK) return -1;

    if (rfm69_run_osc_calibration(&radio, 50000u) != RFM69_OK) return -1;
    if (rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US) != RFM69_OK)
        return -1;
    return 0;
}

int phy_tune(uint32_t hz) {
    return (rfm69_set_carrier_hz(&radio, hz) == RFM69_OK) ? 0 : -1;
}

int phy_standby(void) {
    rfm69_status_t st = rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY,
                                                MODE_TIMEOUT_US);

    /* The flag clears with the mode; so must its shadow, or the next rise is lost. */
    sync_was_set = 0;
    return (st == RFM69_OK) ? 0 : -1;
}

int phy_listen(void) {
    return (rfm69_set_mode(&radio, RFM69_MODE_RX) == RFM69_OK) ? 0 : -1;
}

/* air_us is in this part's ticks, not calibrated us; the caller scales.
 * radio_devices_docs/radio/phy-seam.md */
int phy_transmit(const void *payload, uint8_t len, uint32_t *air_us) {
    rfm69_status_t st;
    uint32_t t0;

    if (len > PHY_MAX_PAYLOAD)
        return -2;
    tx_buffer[0] = len;
    memcpy(tx_buffer + 1, payload, len);

    t0 = rfm_micros();
    /* Out of RX before the FIFO is touched, where no caller can omit it.
     * radio_devices_docs/open_hub/radio/configuration.md */
    st = rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
    if (st != RFM69_OK) return -1;
    st = rfm69_set_fifo_threshold(&radio, 1, 0);
    if (st != RFM69_OK) return -1;
    st = rfm69_write_fifo(&radio, tx_buffer, (uint8_t)(len + 1u));
    if (st != RFM69_OK) return -1;
    st = rfm69_set_mode(&radio, RFM69_MODE_TX);
    if (st != RFM69_OK) return -1;

    /* PacketSent from RegIrqFlags2 rather than polling DIO0 forever. */
    st = rfm69_wait_irq2(&radio, RFM69_IRQ2_PACKET_SENT, TX_TIMEOUT_US);
    if (st == RFM69_OK && air_us != NULL)
        *air_us = rfm_micros() - t0;
    (void)rfm69_set_mode_blocking(&radio, RFM69_MODE_STANDBY, MODE_TIMEOUT_US);
    return (st == RFM69_OK) ? 0 : -1;
}

/* Stamps TIM2 on the DIO3 SyncAddressMatch edge, and nothing above the seam.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    if (pin != RFM_DIO3_Pin)
        return;
    sync_edge_tk = rfm_micros();
    sync_edge_seq++;
}

/* AutoRxRestart waits for an empty FIFO, so bytes left behind end the window.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void restart_rx(void) {
    (void)phy_standby();
    (void)phy_listen();
}

/* A frame failing CRC still occupies the FIFO and has to be read out. */
static void drain_frame(void) {
    uint8_t len = 0;

    if (rfm69_read_fifo(&radio, &len, 1) != RFM69_OK)
        return;
    /* A corrupt length byte is what a failed CRC produces. */
    if (len == 0u || len > PHY_MAX_PAYLOAD) {
        restart_rx();
        return;
    }
    if (rfm69_read_fifo(&radio, drain_buffer, len) != RFM69_OK)
        restart_rx();
}

/* One event per call, measured in the order the part allows.
 * radio_devices_docs/radio/phy-seam.md */
int phy_poll(phy_ev_t *ev) {
    uint8_t flags1 = 0, flags2 = 0;
    uint8_t now, rose, gain = PHY_LNA_UNKNOWN;
    int16_t steps = 0, x2 = 0;
    uint8_t len = 0;

    if (ev == NULL)
        return -2;
    memset(ev, 0, sizeof(*ev));
    ev->kind     = PHY_EV_NONE;
    ev->lna_gain = PHY_LNA_UNKNOWN;

    if (rfm69_get_irq_flags(&radio, &flags1, &flags2) != RFM69_OK) {
        restart_rx();
        return -1;
    }

    now  = (flags1 & RFM69_IRQ1_SYNC_ADDR_MATCH) ? 1u : 0u;
    rose = (now && !sync_was_set) ? 1u : 0u;
    sync_was_set = now;
    ev->busy = now;

    /* Reported on every poll, so an edge no poll saw as a SYNC is still visible. */
    ev->sync_seq   = sync_edge_seq;
    ev->sync_us    = sync_edge_tk;
    ev->sync_valid = (sync_edge_seq != 0u) ? 1u : 0u;

    if (flags2 & RFM69_IRQ2_PAYLOAD_READY) {
        /* Before the CRC branch: the corrupt frames are the population AFC is for.
         * radio_devices_docs/open_hub/radio/configuration.md */
        if (rfm69_get_afc_raw(&radio, &steps) == RFM69_OK) {
            ev->afc_hz    = rfm69_steps_to_hz(steps);
            ev->afc_valid = 1u;
        }
        /* Read here, not later: the AGC settles back to G1 as soon as the air is idle. */
        if (rfm69_get_lna_gain(&radio, &gain) == RFM69_OK)
            ev->lna_gain = gain;
        /* The latch holds this packet's own level only until the receiver re-arms. */
        if (rfm69_get_rssi(&radio, &x2) == RFM69_OK)
            ev->rssi_dbm = (int16_t)(x2 / 2);

        if (!(flags2 & RFM69_IRQ2_CRC_OK)) {
            ev->kind = PHY_EV_CRC;
            drain_frame();
            return 0;
        }
        if (rfm69_read_fifo(&radio, &len, 1) != RFM69_OK) {
            restart_rx();
            return -1;
        }
        if (len == 0u || len > PHY_MAX_PAYLOAD) {
            restart_rx();
            return -1;
        }
        if (rfm69_read_fifo(&radio, ev->buf, len) != RFM69_OK) {
            restart_rx();
            return -1;
        }
        ev->len  = len;
        ev->kind = PHY_EV_FRAME;
        return 0;
    }

    if (rose) {
        /* The latch, not a trigger: a trigger never completes while sync is high.
         * radio_devices_docs/open_hub/radio/configuration.md */
        if (rfm69_get_rssi(&radio, &x2) == RFM69_OK)
            ev->rssi_dbm = (int16_t)(x2 / 2);
        ev->kind = PHY_EV_SYNC;
    }
    return 0;
}

/* TIM2's counter; a tick here is not a calibrated microsecond.
 * radio_devices_docs/radio/phy-seam.md */
uint32_t phy_now_us(void) {
    return rfm_micros();
}

/* Triggered, and refused while a frame is arriving: it destroys the latch.
 * radio_devices_docs/open_hub/radio/configuration.md */
int16_t phy_rssi_now(void) {
    int16_t x2 = 0;

    if (sync_was_set)
        return 0;
    if (rfm69_measure_rssi(&radio, RSSI_TIMEOUT_US, &x2) != RFM69_OK)
        return 0;
    return (int16_t)(x2 / 2);
}
