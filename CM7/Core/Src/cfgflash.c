/**
 * @file cfgflash.c
 * @brief Erase, program and verify for the configuration store's own sectors.
 *
 * ADR-0027 §7. radio_devices_docs/open_hub/arch/config-store.md
 */
#include "cfgflash.h"

#include <string.h>

#include "cfgstore.h"
#include "cmsis_os.h"
#include "hsem_table.h"

#define CFGF_WORD_BYTES  32u

extern uint32_t _sitcmfunc, _eitcmfunc, _siitcmfunc;

static uint32_t itcm_bytes;

/* Placed in ITCM and calling nothing: the HAL is in the bank being erased. */
__attribute__((section(".itcmfunc"), noinline))
static uint32_t erase_from_itcm(uint32_t sector, volatile uint32_t *spins)
{
    CFGFLASH_ERASE_BODY(sector, spins);
}

uint32_t cfgflash_init(void)
{
    itcm_bytes = (uint32_t)((uint8_t *)&_eitcmfunc - (uint8_t *)&_sitcmfunc);
    if (itcm_bytes == 0u)
        return 0u;
    memcpy((void *)&_sitcmfunc, (const void *)&_siitcmfunc, (size_t)itcm_bytes);
    SCB_InvalidateICache();
    return itcm_bytes;
}

uint32_t cfgflash_sector_addr(uint8_t sector)
{
    switch (sector) {
    case CFG_IDENTITY_SECTOR:  return CFG_IDENTITY_ADDR;
    case CFG_JOURNAL_SECTOR_A: return CFG_JOURNAL_ADDR_A;
    case CFG_JOURNAL_SECTOR_B: return CFG_JOURNAL_ADDR_B;
    default:                   return 0u;
    }
}

int cfgflash_is_erased(uint32_t addr, uint32_t bytes)
{
    const volatile uint32_t *w = (const volatile uint32_t *)addr;

    for (uint32_t i = 0; i < bytes / 4u; i++) {
        if (w[i] != 0xFFFFFFFFu)
            return 0;
    }
    return 1;
}

/* Only the three the store owns; a check rather than a rule.
 * radio_devices_docs/open_hub/arch/config-store.md */
static int sector_is_ours(uint8_t sector)
{
    return cfgflash_sector_addr(sector) != 0u;
}

cfgflash_err_t cfgflash_erase(uint8_t sector, uint32_t *ms_out)
{
    uint32_t prim, t0, spins = 0, sr;

    if (!sector_is_ours(sector))
        return CFGF_ERR_SECTOR;
    /* CM4 arms IWDG2 before it waits on this, and 954 ms outlasts the period.
     * radio_devices_docs/open_hub/arch/dual-core.md */
    if (HAL_HSEM_IsSemTaken(HSEM_ID_0))
        return CFGF_ERR_CM4_HELD;
    /* Interrupts are masked for the duration, which no running scheduler wants. */
    if (osKernelGetState() == osKernelRunning)
        return CFGF_ERR_SCHEDULER;
    if (itcm_bytes == 0u && cfgflash_init() == 0u)
        return CFGF_ERR_NO_ITCM;

    /* DWT, not HAL_GetTick(): masked interrupts stop the tick, so 954 ms read 1.
     * radio_devices_docs/open_hub/arch/config-store.md */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Cleans first: a bare invalidate discards this function's own stack. */
    SCB_DisableDCache();
    prim = __get_PRIMASK();
    __disable_irq();
    t0 = DWT->CYCCNT;
    sr = erase_from_itcm(sector, &spins);
    t0 = DWT->CYCCNT - t0;
    __set_PRIMASK(prim);
    SCB_EnableDCache();

    if (ms_out != NULL) {
        uint32_t khz = SystemCoreClock / 1000u;

        *ms_out = khz ? (t0 / khz) : 0u;
    }
    /* EOP alone. Any error bit means the sector is not to be trusted. */
    if ((sr & ~(uint32_t)FLASH_SR_EOP) != 0u)
        return CFGF_ERR_PROGRAM;
    return CFGF_OK;
}

cfgflash_err_t cfgflash_program(uint32_t addr, const void *src, uint32_t bytes)
{
    uint32_t base = 0;

    if ((addr % CFGF_WORD_BYTES) != 0u || (bytes % CFGF_WORD_BYTES) != 0u ||
        bytes == 0u)
        return CFGF_ERR_ALIGN;

    for (uint8_t s = 0; s < 8u; s++) {
        uint32_t a = cfgflash_sector_addr(s);

        if (a != 0u && addr >= a && addr + bytes <= a + CFG_SECTOR_BYTES) {
            base = a;
            break;
        }
    }
    if (base == 0u)
        return CFGF_ERR_RANGE;
    /* Overwriting a non-virgin word leaves a permanent ECC code. RM0399 4.3.9. */
    if (!cfgflash_is_erased(addr, bytes))
        return CFGF_ERR_NOT_ERASED;

    /* An erase returns CR1 locked with SER set; the next program fails until both
     * are cleared. radio_devices_docs/open_hub/arch/config-store.md */
    if ((FLASH->CR1 & FLASH_CR_LOCK) == 0u)
        (void)HAL_FLASH_Lock();
    if (HAL_FLASH_Unlock() != HAL_OK)
        return CFGF_ERR_UNLOCK;
    FLASH->CR1 &= ~FLASH_CR_SER;
    FLASH->CCR1 = 0xFFFFFFFFu;

    for (uint32_t off = 0; off < bytes; off += CFGF_WORD_BYTES) {
        uint8_t word[CFGF_WORD_BYTES] __attribute__((aligned(32)));

        memcpy(word, (const uint8_t *)src + off, CFGF_WORD_BYTES);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + off,
                              (uint64_t)(uint32_t)word) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return CFGF_ERR_PROGRAM;
        }
    }
    (void)HAL_FLASH_Lock();

    /* A write that did not land reads the same as one that did, until something
     * looks. radio_devices_docs/open_hub/arch/config-store.md */
    if (memcmp((const void *)addr, src, bytes) != 0)
        return CFGF_ERR_VERIFY;
    return CFGF_OK;
}

const char *cfgflash_err_str(cfgflash_err_t e)
{
    switch (e) {
    case CFGF_OK:              return "ok";
    case CFGF_ERR_SECTOR:      return "not one of the store's sectors";
    case CFGF_ERR_CM4_HELD:    return "CM4 still holds the boot semaphore";
    case CFGF_ERR_SCHEDULER:   return "the scheduler is running";
    case CFGF_ERR_UNLOCK:      return "the flash would not unlock";
    case CFGF_ERR_ALIGN:       return "not whole flash words";
    case CFGF_ERR_RANGE:       return "outside the sector it started in";
    case CFGF_ERR_NOT_ERASED:  return "the target is not virgin";
    case CFGF_ERR_PROGRAM:     return "the part reported an error";
    case CFGF_ERR_VERIFY:      return "programmed and read back different";
    case CFGF_ERR_NO_ITCM:     return "the erase routine is not in ITCM";
    }
    return "unknown";
}

uint32_t cfgflash_selftest(void)
{
    static const uint8_t word[CFGF_WORD_BYTES] __attribute__((aligned(32))) = { 0 };
    uint32_t bad = 0;

    if (cfgflash_init() == 0u)
        bad |= CFGF_ST_ITCM;
    /* Sector 0 holds this image; accepting it would erase the firmware. */
    if (cfgflash_erase(0u, NULL) != CFGF_ERR_SECTOR)
        bad |= CFGF_ST_SECTOR;
    /* Runs from the console, so the scheduler is up and the erase must refuse. */
    if (cfgflash_erase(CFG_JOURNAL_SECTOR_B, NULL) != CFGF_ERR_SCHEDULER)
        bad |= CFGF_ST_SCHEDULER;
    if (cfgflash_program(CFG_JOURNAL_ADDR_A + 1u, word, CFGF_WORD_BYTES) != CFGF_ERR_ALIGN)
        bad |= CFGF_ST_ALIGN;
    if (cfgflash_program(CFG_JOURNAL_ADDR_A, word, 8u) != CFGF_ERR_ALIGN)
        bad |= CFGF_ST_ALIGN;
    if (cfgflash_program(0x08000000u, word, CFGF_WORD_BYTES) != CFGF_ERR_RANGE)
        bad |= CFGF_ST_RANGE;
    /* Past our last sector is CM4's bank; a span crossing a sector end is in
     * neither. radio_devices_docs/open_hub/arch/config-store.md */
    if (cfgflash_program(CFG_JOURNAL_ADDR_B + CFG_SECTOR_BYTES, word,
                         CFGF_WORD_BYTES) != CFGF_ERR_RANGE)
        bad |= CFGF_ST_RANGE;
    if (cfgflash_program(CFG_JOURNAL_ADDR_A + CFG_SECTOR_BYTES - CFGF_WORD_BYTES,
                         word, 2u * CFGF_WORD_BYTES) != CFGF_ERR_RANGE)
        bad |= CFGF_ST_RANGE;
    /* Needs something written to overwrite; an erased sector cannot show it. */
    if (cfgflash_is_erased(CFG_JOURNAL_ADDR_A, CFGF_WORD_BYTES))
        bad |= CFGF_ST_NO_POP;
    else if (cfgflash_program(CFG_JOURNAL_ADDR_A, word,
                              CFGF_WORD_BYTES) != CFGF_ERR_NOT_ERASED)
        bad |= CFGF_ST_NOT_ERASED;
    /* Distinct conditions must not render as one word. */
    if (cfgflash_err_str(CFGF_ERR_SECTOR) == cfgflash_err_str(CFGF_ERR_RANGE) ||
        strcmp(cfgflash_err_str(CFGF_ERR_SECTOR), cfgflash_err_str(CFGF_ERR_RANGE)) == 0)
        bad |= CFGF_ST_STRINGS;
    return bad;
}
