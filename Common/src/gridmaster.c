/**
 * @file gridmaster.c
 * @brief Bootstrap and period installation; the stepping itself is grid.c.
 *
 * radio_devices_docs/radio/tdma.md
 */
#include "gridmaster.h"

int gridmaster_service(gridmaster_t *m, uint32_t now, uint32_t period) {
    if (!m->started) {
        grid_start(&m->g, m->g.counter, period, now);
        m->started = 1;
        return 0;
    }
    if (grid_advance(&m->g, now) == 0u)
        return 0;

    /* Installed after the boundary, so a moved period moves the next interval. */
    m->g.period = period;
    return 1;
}
