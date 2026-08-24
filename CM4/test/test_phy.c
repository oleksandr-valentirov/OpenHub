/**
 * @file test_phy.c
 * @brief The receive path against a part that is not there, which is the point.
 *
 * Every rule this checks was learned on hardware and none of them is visible to
 * a compiler: an edge counted as a level, a triggered measurement destroying the
 * latch it was meant to read, a carrier error read after the FIFO drain has
 * already re-armed the receiver. The seam exists so that these can fail in a
 * second on a PC instead of over an hour on the air.
 *
 * radio_devices_docs/radio/phy-seam.md
 * radio_devices_docs/open_hub/testing/host-tests.md
 */
#include <stdio.h>
#include <string.h>

#include "fake_part.h"
#include "phy.h"
#include "phy_rfm69.h"
#include "main.h"
#include "timebase.h"

static int fails;
static unsigned checks;

/* Which stimulus is being corrupted, so the suite can prove it can go red. */
enum { MUT_NONE = 0, MUT_NO_SYNC, MUT_CRC_ALWAYS_OK, MUT_SHORT_FIFO };
static int mutation;

/* A refusal under a mutation is the result, so it is not printed as a failure. */
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        if (!mutation) \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        fails++; \
    } \
} while (0)

static fake_part_t part;
static uint32_t clock_us;

/* --- the board, as the shims declare it --- */

void HAL_GPIO_WritePin(void *port, uint16_t pin, int state) {
    (void)port;
    if (pin == RFM_CS_Pin)
        part.chip.selected = (state == GPIO_PIN_RESET);
}

int rfm_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    clock_us += 2;
    return fake_part_transfer(&part, tx, rx, len);
}

uint32_t rfm_micros(void) { return ++clock_us; }

void delay_us_poll(uint32_t us) { clock_us += us; }

/* --- the part, driven --- */

static void air_quiet(void) {
    part.chip.regs[RFM69_RegIrqFlags1] &= (uint8_t)~RFM69_IRQ1_SYNC_ADDR_MATCH;
    part.chip.regs[RFM69_RegIrqFlags2] &=
        (uint8_t)~(RFM69_IRQ2_PAYLOAD_READY | RFM69_IRQ2_CRC_OK);
}

static void air_sync(void) {
    if (mutation == MUT_NO_SYNC)
        return;
    part.chip.regs[RFM69_RegIrqFlags1] |= RFM69_IRQ1_SYNC_ADDR_MATCH;
}

static void air_frame(const uint8_t *payload, uint8_t len, int crc_ok) {
    fake_part_load(&part, payload, len);
    if (mutation == MUT_SHORT_FIFO && part.rx_len > 1u)
        part.rx_len--;
    part.chip.regs[RFM69_RegIrqFlags2] |= RFM69_IRQ2_PAYLOAD_READY;
    if (crc_ok || mutation == MUT_CRC_ALWAYS_OK)
        part.chip.regs[RFM69_RegIrqFlags2] |= RFM69_IRQ2_CRC_OK;
    else
        part.chip.regs[RFM69_RegIrqFlags2] &= (uint8_t)~RFM69_IRQ2_CRC_OK;
}

/* A frame ends when the part has handed it up, flags and all. */
static void air_frame_taken(void) {
    part.chip.regs[RFM69_RegIrqFlags2] &=
        (uint8_t)~(RFM69_IRQ2_PAYLOAD_READY | RFM69_IRQ2_CRC_OK);
}

static void reset_part(void) {
    fake_part_init(&part);
    clock_us = 1000;
    CHECK(phy_init() == 0);
    CHECK(phy_listen() == 0);
    part.log_n = 0;
}

/* --- the cases --- */

/* Silence is an event with a kind, not an absence of one. */
static void case_quiet(void) {
    phy_ev_t ev;

    air_quiet();
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_NONE);
    CHECK(ev.len == 0);
    CHECK(ev.busy == 0);
}

/* A level, so a poll counts the rise or it counts one frame hundreds of times.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void case_sync_is_an_edge(void) {
    phy_ev_t ev;
    unsigned syncs = 0;
    int i;

    air_quiet();
    (void)phy_poll(&ev);
    air_sync();
    for (i = 0; i < 20; i++) {
        CHECK(phy_poll(&ev) == 0);
        if (ev.kind == PHY_EV_SYNC)
            syncs++;
        CHECK(ev.busy == (mutation == MUT_NO_SYNC ? 0u : 1u));
    }
    CHECK(syncs == 1u);
    air_quiet();
    (void)phy_poll(&ev);
}

/* The frame's level is the latch; a trigger completes on the air instead.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void case_level_is_the_latch(void) {
    phy_ev_t ev;
    int i;
    int seen = 0;

    air_quiet();
    (void)phy_poll(&ev);
    part.chip.regs[RFM69_RegRssiValue] = 140u;   /* the latch: -70 dBm */
    part.chip.rssi_air = 40u;                    /* the air now:  -20 dBm */
    air_sync();
    for (i = 0; i < 4 && !seen; i++) {
        CHECK(phy_poll(&ev) == 0);
        if (ev.kind == PHY_EV_SYNC) {
            seen = 1;
            CHECK(ev.rssi_dbm == -70);
        }
    }
    CHECK(seen == 1);
    air_quiet();
    (void)phy_poll(&ev);
}

/* A whole frame, and the bytes are the ones the part held. */
static void case_frame(void) {
    static const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    phy_ev_t ev;

    air_quiet();
    (void)phy_poll(&ev);
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_FRAME);
    CHECK(ev.len == sizeof(payload));
    CHECK(memcmp(ev.buf, payload, sizeof(payload)) == 0);
    air_frame_taken();
}

/* CrcAutoClearOff delivers the corrupt frame, so it has to be read out.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void case_crc_failure_is_drained(void) {
    static const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    phy_ev_t ev;

    air_quiet();
    (void)phy_poll(&ev);
    air_frame(payload, (uint8_t)sizeof(payload), 0);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_CRC);
    CHECK(ev.len == 0);
    /* Every byte the part held was taken, length byte included. */
    CHECK(part.rx_pos == part.rx_len);
    air_frame_taken();
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_NONE);
}

/* AutoRxRestart re-arms on the drain, so both are gone by then.
 * radio_devices_docs/open_hub/radio/configuration.md */
static void case_measured_before_the_drain(void) {
    static const uint8_t payload[] = {0x01, 0x02};
    phy_ev_t ev;
    int afc, lna, fifo;

    air_quiet();
    (void)phy_poll(&ev);
    part.log_n = 0;
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    afc  = fake_part_first_read(&part, RFM69_RegAfcMsb);
    lna  = fake_part_first_read(&part, RFM69_RegLna);
    fifo = fake_part_first_read(&part, RFM69_RegFifo);
    CHECK(afc >= 0);
    CHECK(lna >= 0);
    CHECK(fifo >= 0);
    CHECK(afc < fifo);
    CHECK(lna < fifo);
    air_frame_taken();
}

/* A failed read must not enter a fit as a zero, and 0 Hz is an ordinary error. */
static void case_afc_refused(void) {
    static const uint8_t payload[] = {0x07};
    phy_ev_t ev;

    air_quiet();
    (void)phy_poll(&ev);
    part.afc_refuse = 1;
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_FRAME);
    CHECK(ev.afc_valid == 0);
    part.afc_refuse = 0;
    air_frame_taken();

    /* And the ordinary case reports a value with the flag set. */
    air_quiet();
    (void)phy_poll(&ev);
    part.chip.regs[RFM69_RegAfcMsb] = 0x01;
    part.chip.regs[RFM69_RegAfcMsb + 1u] = 0x00;
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.afc_valid == 1);
    CHECK(ev.afc_hz == rfm69_steps_to_hz(256));
    air_frame_taken();
}

/* Unknown is 0xFF and not G1, which is a gain the AGC really does choose. */
static void case_gain_unknown(void) {
    static const uint8_t payload[] = {0x09};
    phy_ev_t ev;

    air_quiet();
    (void)phy_poll(&ev);
    part.lna_refuse = 1;
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.lna_gain == PHY_LNA_UNKNOWN);
    part.lna_refuse = 0;
    air_frame_taken();

    air_quiet();
    (void)phy_poll(&ev);
    part.chip.regs[RFM69_RegLna] = (uint8_t)(3u << 3);
    air_frame(payload, (uint8_t)sizeof(payload), 1);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.lna_gain == 3u);
    air_frame_taken();
}

/* The pin's own counter, reported whatever the poll found.
 * radio_devices_docs/radio/phy-seam.md */
static void case_sync_seq(void) {
    phy_ev_t ev;
    uint32_t before;

    air_quiet();
    CHECK(phy_poll(&ev) == 0);
    before = ev.sync_seq;
    HAL_GPIO_EXTI_Callback(RFM_DIO3_Pin);
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.kind == PHY_EV_NONE);
    CHECK(ev.sync_seq == before + 1u);
    CHECK(ev.sync_valid == 1u);
    /* A pin that is not DIO3 is not this radio's edge. */
    HAL_GPIO_EXTI_Callback((uint16_t)(RFM_DIO3_Pin ^ 0xFFFFu));
    CHECK(phy_poll(&ev) == 0);
    CHECK(ev.sync_seq == before + 1u);
}

/* A corrupt length byte is what a failed CRC produces, and it must not be
 * believed. radio_devices_docs/open_hub/radio/configuration.md */
static void case_bad_length(void) {
    phy_ev_t ev;
    uint8_t big[PHY_MAX_PAYLOAD];

    air_quiet();
    (void)phy_poll(&ev);
    memset(big, 0x5A, sizeof(big));
    fake_part_load(&part, big, (uint8_t)sizeof(big));
    part.rx[0] = 0;                       /* the length the part hands up */
    part.chip.regs[RFM69_RegIrqFlags2] |= RFM69_IRQ2_PAYLOAD_READY |
                                          RFM69_IRQ2_CRC_OK;
    CHECK(phy_poll(&ev) != 0);
    CHECK(ev.kind != PHY_EV_FRAME);
    air_frame_taken();
}

/* A trigger completes on the air, so it may not run while a frame is arriving. */
static void case_floor_refuses_mid_frame(void) {
    phy_ev_t ev;

    air_quiet();
    (void)phy_poll(&ev);
    part.chip.rssi_air = 190u;            /* the band: -95 dBm */
    CHECK(phy_rssi_now() == -95);
    air_sync();
    CHECK(phy_poll(&ev) == 0);
    if (mutation != MUT_NO_SYNC)
        CHECK(phy_rssi_now() == 0);
    air_quiet();
    (void)phy_poll(&ev);
}

/* The length byte and the payload are one write, which is what the part needs. */
static void case_transmit(void) {
    static const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t air = 0;

    part.chip.fifo_len = 0;
    part.chip.regs[RFM69_RegIrqFlags2] |= RFM69_IRQ2_PACKET_SENT;
    CHECK(phy_transmit(payload, (uint8_t)sizeof(payload), &air) == 0);
    CHECK(part.chip.fifo_len == sizeof(payload) + 1u);
    CHECK(part.chip.fifo[0] == sizeof(payload));
    CHECK(memcmp(part.chip.fifo + 1, payload, sizeof(payload)) == 0);
    CHECK(air > 0u);
    part.chip.regs[RFM69_RegIrqFlags2] &= (uint8_t)~RFM69_IRQ2_PACKET_SENT;
    CHECK(phy_transmit(payload, PHY_MAX_PAYLOAD + 1u, &air) == -2);
    CHECK(phy_listen() == 0);
}

static void run_all(void) {
    reset_part();
    case_quiet();
    case_sync_is_an_edge();
    case_level_is_the_latch();
    case_frame();
    case_crc_failure_is_drained();
    case_measured_before_the_drain();
    case_afc_refused();
    case_gain_unknown();
    case_sync_seq();
    case_bad_length();
    case_floor_refuses_mid_frame();
    case_transmit();
}

/* A suite that has never refused reads in neither direction.
 * radio_devices_docs/open_hub/testing/host-tests.md */
static int mutation_goes_red(int which, const char *name) {
    int before = fails;
    int red;

    mutation = which;
    fails = 0;
    run_all();
    red = fails;
    if (!red)
        printf("FAIL mutation '%s' passed, so the checks above prove nothing\n", name);
    else
        printf("  mutation '%s': %d check(s) refused it\n", name, red);
    mutation = MUT_NONE;
    fails = before + (red ? 0 : 1);
    return red;
}

int main(void) {
    unsigned clean_checks;

    run_all();
    clean_checks = checks;
    /* An empty population is not a pass: a deleted case must be visible here. */
    if (clean_checks < 60u) {
        printf("FAIL only %u checks ran, which is fewer than this suite has\n",
               clean_checks);
        fails++;
    }
    if (fails == 0) {
        (void)mutation_goes_red(MUT_NO_SYNC, "no sync edge");
        (void)mutation_goes_red(MUT_CRC_ALWAYS_OK, "CRC always ok");
        (void)mutation_goes_red(MUT_SHORT_FIFO, "one byte short");
    }
    printf("phy: %s (%u checks, 3 mutation controls)\n",
           fails ? "FAIL" : "ok", clean_checks);
    return fails ? 1 : 0;
}
