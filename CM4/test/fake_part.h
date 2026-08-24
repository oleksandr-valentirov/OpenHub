#pragma once

/**
 * @file fake_part.h
 * @brief A receiving RFM69, built on the driver's own fake rather than beside it.
 *
 * `rfm69_lib/test/fake_spi.h` models a register file and the RSSI latch, which
 * is what testing the driver needs. Testing the layer above needs a part that
 * also *delivers frames*: a FIFO that pops, an AFC pair, a gain field and reads
 * that can be made to refuse. Those are added here by wrapping its transfer,
 * because the driver's fake is another repository's file.
 *
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdint.h>
#include <string.h>

#include "fake_spi.h"

/* Every register touched, in order, so an ordering rule can be a check. */
#define FAKE_LOG_MAX  512u

typedef struct {
    fake_chip_t chip;
    uint8_t  rx[80];              /* what the part will hand up, length byte first */
    uint8_t  rx_len, rx_pos;
    uint8_t  log_reg[FAKE_LOG_MAX];
    uint8_t  log_wr[FAKE_LOG_MAX];
    uint16_t log_n;
    uint8_t  afc_refuse;          /* the AFC pair read fails while this is set */
    uint8_t  lna_refuse;          /* ... and the gain read, independently */
    uint32_t now_us;
} fake_part_t;

/* Loads one frame for the part to deliver, length byte and all. */
static void fake_part_load(fake_part_t *p, const uint8_t *payload, uint8_t len) {
    p->rx[0] = len;
    memcpy(p->rx + 1, payload, len);
    p->rx_len = (uint8_t)(len + 1u);
    p->rx_pos = 0;
}

static void fake_part_log(fake_part_t *p, uint8_t reg, uint8_t wr) {
    if (p->log_n >= FAKE_LOG_MAX)
        return;
    p->log_reg[p->log_n] = reg;
    p->log_wr[p->log_n]  = wr;
    p->log_n++;
}

/* The first index at which reg was read, or -1. */
static int fake_part_first_read(const fake_part_t *p, uint8_t reg) {
    uint16_t i;

    for (i = 0; i < p->log_n; i++)
        if (p->log_reg[i] == reg && !p->log_wr[i])
            return (int)i;
    return -1;
}

static int fake_part_transfer(fake_part_t *p, const uint8_t *tx, uint8_t *rx,
                              size_t len) {
    uint8_t addr = tx[0] & 0x7Fu;
    uint8_t wr   = (tx[0] & 0x80u) ? 1u : 0u;
    size_t i;

    fake_part_log(p, addr, wr);

    if (!wr) {
        /* Always ready and calibrated, or every blocking call times out. */
        p->chip.regs[RFM69_RegIrqFlags1] |= RFM69_IRQ1_MODE_READY;
        p->chip.regs[RFM69_RegOsc1]      |= 0x40u;

        if (addr == RFM69_RegAfcMsb && p->afc_refuse)
            return -1;
        if (addr == RFM69_RegLna && p->lna_refuse)
            return -1;
        /* A FIFO read pops, which is the whole difference from a register. */
        if (addr == RFM69_RegFifo && rx != NULL) {
            rx[0] = 0;
            for (i = 1; i < len; i++)
                rx[i] = (p->rx_pos < p->rx_len) ? p->rx[p->rx_pos++] : 0u;
            return 0;
        }
    }
    return fake_transfer(&p->chip, tx, rx, len);
}

/* The part as it comes out of reset, with nothing loaded and nothing flagged. */
static void fake_part_init(fake_part_t *p) {
    rfm69_io_t unused;

    memset(p, 0, sizeof(*p));
    fake_init(&p->chip, &unused);
    (void)unused;
}
