/**
 * @file grid.c
 * @brief The stepping rule, and only the rule.
 *
 * radio_devices_docs/radio/tdma.md
 */
#include "grid.h"

void grid_start(grid_t *g, uint32_t counter, uint32_t period, uint32_t now) {
    g->start   = now;
    g->period  = period;
    g->counter = counter;
    g->running = 1;
}

uint32_t grid_advance(grid_t *g, uint32_t now) {
    uint32_t crossed = 0;

    /* A zero period never reaches its own next boundary, so it never returns. */
    if (!g->running || g->period == 0u)
        return 0;
    /* Signed, so a boundary straddling the counter's wrap still compares right. */
    while ((int32_t)(now - (g->start + g->period)) >= 0) {
        g->start += g->period;
        g->counter++;
        crossed++;
    }
    return crossed;
}

uint32_t grid_offset(const grid_t *g, uint32_t now) {
    return now - g->start;
}
