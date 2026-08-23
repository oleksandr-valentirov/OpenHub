/**
 * @file hub_boot.h
 * @brief The boot handshake's timing contract between the two cores.
 *
 * CM4 waits on HSEM_ID_0 while CM7 configures the shared clock tree, and it has
 * already armed IWDG2 by then. The wait therefore outlives the watchdog period
 * and must refresh it. radio_devices_docs/open_hub/arch/dual-core.md
 */
#ifndef HUB_BOOT_H
#define HUB_BOOT_H

#include <assert.h>
#include <stdint.h>

/* IWDG_PRESCALER_4 and Reload 4095 at the nominal 32 kHz LSI. */
#define HUB_IWDG2_PERIOD_MS       512u

/* The same at the fastest LSI the tolerance allows, which nobody has read off
 * the datasheet. radio_devices_docs/open_hub/arch/dual-core.md */
#define HUB_IWDG2_PERIOD_MS_MIN   348u

/* How long CM7 may hold HSEM_ID_0. Declared, not enforced: CM4 reports what it
 * waited. radio_devices_docs/open_hub/arch/dual-core.md */
#define HUB_CM7_BOOT_BUDGET_MS   3000u

_Static_assert(HUB_IWDG2_PERIOD_MS_MIN < HUB_IWDG2_PERIOD_MS,
               "the fast-LSI period is the short one");
/* Fires if CM7's boot comes inside the watchdog, which would make the refresh
 * dead code. radio_devices_docs/open_hub/arch/dual-core.md */
_Static_assert(HUB_CM7_BOOT_BUDGET_MS > HUB_IWDG2_PERIOD_MS_MIN,
               "CM4's wait outlives IWDG2, which is why it refreshes");

#endif /* HUB_BOOT_H */
