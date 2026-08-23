#pragma once

#include <stdint.h>

/**
 * @file erasetest.h
 * @brief Can this core erase a sector of the bank it runs from, and from where?
 *
 * Sector 3 of bank 1: nothing occupies it, so a half-erased result is read by
 * nothing. Two arms, ITCM and AXI SRAM, each with a pattern to undo.
 * radio_devices_docs/open_hub/arch/config-store.md
 */

/** What the run observed. `mark` advances step by step so a death localises. */
typedef struct erasetest {
    uint8_t  ran;          /**< the routine was entered at all */
    uint8_t  mark;         /**< last step reached; 0xFF is finished */
    uint8_t  prog_a;       /**< pattern laid before arm A; 2 = it was already there */
    uint8_t  prog_b;
    uint8_t  erased_a;     /**< arm A, from ITCM, erased all three probes */
    uint8_t  erased_b;     /**< arm B, from AXI SRAM */
    uint8_t  pad[2];
    uint32_t rsr;          /**< RCC_RSR at entry: what reset the core last time */
    uint32_t cr1_before;
    uint32_t sr_before;
    uint32_t cr1_a;
    uint32_t sr_a;
    uint32_t cr1_b;
    uint32_t sr_b;
    uint32_t cycles_a;     /**< DWT counts core clocks through a stall */
    uint32_t cycles_b;
    uint32_t spins_a;      /**< polls of QW1, which only a running core makes */
    uint32_t spins_b;
    uint32_t cpu_hz;
} erasetest_t;

/** @brief Runs the measurement once, before the scheduler. */
void erasetest_run(void);

/** @brief Copies out what it observed. @param out receives the result */
void erasetest_get(erasetest_t *out);
