/**
 * @file bootwait.c
 * @brief The wait CM4 does on CM7's boot, and the only place it is measured.
 *
 * This wait is what cost the board twice: it did not refresh IWDG2, so an erase
 * held on the far side of it reset the system mid-flight and left the sector
 * faulting on every read. radio_devices_docs/open_hub/arch/dual-core.md
 */
#include "bootwait.h"
#include "hsem_table.h"
#include "hub_boot.h"

static uint32_t wait_ms;
static uint32_t wait_spins;

void bootwait_for_cm7(IWDG_HandleTypeDef *iwdg)
{
    uint32_t start = HAL_GetTick();

    /* Unconditional, not paced off HAL_GetTick(): safety must not rest on the
     * instrument. radio_devices_docs/open_hub/arch/dual-core.md */
    while (HAL_HSEM_IsSemTaken(HSEM_ID_0)) {
        if (iwdg != NULL)
            HAL_IWDG_Refresh(iwdg);
        if (wait_spins != 0xFFFFFFFFu)
            wait_spins++;
    }

    wait_ms = HAL_GetTick() - start;
}

uint32_t bootwait_ms(void)
{
    return wait_ms;
}

uint32_t bootwait_spins(void)
{
    return wait_spins;
}
