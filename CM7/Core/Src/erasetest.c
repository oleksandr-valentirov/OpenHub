#include "erasetest.h"

#include <string.h>

#include "main.h"
#include "cfgflash.h"

/* 128 KB nothing reads, so an interrupted erase is harmless.
 * radio_devices_docs/open_hub/arch/config-store.md */
#define ET_SECTOR   FLASH_SECTOR_3
#define ET_ADDR     0x08060000u
#define ET_MID      (ET_ADDR + 0x10000u)
#define ET_END      (ET_ADDR + 0x1FFE0u)
#define ET_WORD     32u

/* Read by SWD after a death; run 1 reported only through a console it killed. */
volatile erasetest_t g_erasetest;

extern uint32_t _sitcmfunc, _eitcmfunc, _siitcmfunc;

static const uint8_t pattern[ET_WORD] __attribute__((aligned(32))) = {
    0xA5, 0x5A, 0xC3, 0x3C, 0x0F, 0xF0, 0x69, 0x96,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE
};

static int word_is_erased(uint32_t addr) {
    const volatile uint32_t *w = (const volatile uint32_t *)addr;

    for (unsigned i = 0; i < ET_WORD / 4u; i++)
        if (w[i] != 0xFFFFFFFFu)
            return 0;
    return 1;
}

/* The driver's body, not a copy: 954 ms is quoted as describing the store.
 * radio_devices_docs/open_hub/arch/config-store.md */
__attribute__((section(".itcmfunc"), noinline))
static uint32_t erase_from_itcm(volatile uint32_t *spins) {
    CFGFLASH_ERASE_BODY(ET_SECTOR, spins);
}

__attribute__((section(".ramfunc"), noinline))
static uint32_t erase_from_ram(volatile uint32_t *spins) {
    CFGFLASH_ERASE_BODY(ET_SECTOR, spins);
}

/* Gives the next erase real work. CR1 comes back locked with SER set. */
static uint8_t lay_pattern(void) {
    if ((FLASH->CR1 & FLASH_CR_LOCK) != 0u) {
        FLASH->KEYR1 = 0x45670123u;
        FLASH->KEYR1 = 0xCDEF89ABu;
    }
    FLASH->CR1 &= ~FLASH_CR_SER;
    FLASH->CCR1 = 0xFFFFFFFFu;
    if (!word_is_erased(ET_ADDR))
        return 2;
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, ET_ADDR,
                          (uint64_t)(uint32_t)pattern) != HAL_OK)
        return 0;
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, ET_MID,
                          (uint64_t)(uint32_t)pattern) != HAL_OK)
        return 0;
    return (uint8_t)(!word_is_erased(ET_ADDR) && !word_is_erased(ET_MID));
}

void erasetest_run(void) {
    uint32_t prim, c0;

    memset((void *)&g_erasetest, 0, sizeof(g_erasetest));
    g_erasetest.ran = 1;
    g_erasetest.mark = 1;
    g_erasetest.cpu_hz = HAL_RCC_GetSysClockFreq();

    /* Cleans first; a bare invalidate discards this function's own stack. */
    SCB_DisableDCache();
    g_erasetest.mark = 2;

    /* .itcmfunc has a load address in flash and no startup loop of its own. */
    memcpy((void *)&_sitcmfunc, (const void *)&_siitcmfunc,
           (size_t)((uint8_t *)&_eitcmfunc - (uint8_t *)&_sitcmfunc));
    SCB_InvalidateICache();
    g_erasetest.mark = 3;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    g_erasetest.rsr = RCC->RSR;
    g_erasetest.cr1_before = FLASH->CR1;
    g_erasetest.sr_before  = FLASH->SR1;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        g_erasetest.mark = 0xE1;
        return;
    }

    /* A clean sector makes "erased" trivially true; both arms lay first. */
    g_erasetest.prog_a = lay_pattern();
    g_erasetest.mark = 4;
    prim = __get_PRIMASK();
    __disable_irq();
    c0 = DWT->CYCCNT;
    g_erasetest.mark = 5;
    g_erasetest.sr_a = erase_from_itcm(&g_erasetest.spins_a);
    g_erasetest.cycles_a = DWT->CYCCNT - c0;
    g_erasetest.mark = 6;
    __set_PRIMASK(prim);
    g_erasetest.cr1_a = FLASH->CR1;
    g_erasetest.erased_a = (uint8_t)(word_is_erased(ET_ADDR) &&
                                     word_is_erased(ET_MID) &&
                                     word_is_erased(ET_END));
    g_erasetest.mark = 7;

    /* --- arm B: AXI SRAM, against a pattern this run laid --- */
    g_erasetest.prog_b = lay_pattern();
    g_erasetest.mark = 8;
    prim = __get_PRIMASK();
    __disable_irq();
    c0 = DWT->CYCCNT;
    g_erasetest.mark = 9;
    g_erasetest.sr_b = erase_from_ram(&g_erasetest.spins_b);
    g_erasetest.cycles_b = DWT->CYCCNT - c0;
    g_erasetest.mark = 10;
    __set_PRIMASK(prim);
    g_erasetest.cr1_b = FLASH->CR1;
    g_erasetest.erased_b = (uint8_t)(word_is_erased(ET_ADDR) &&
                                     word_is_erased(ET_MID) &&
                                     word_is_erased(ET_END));
    g_erasetest.mark = 11;

    (void)HAL_FLASH_Lock();
    SCB_EnableDCache();
    g_erasetest.mark = 0xFF;
}

void erasetest_get(erasetest_t *out) {
    if (out != NULL)
        *out = *(const erasetest_t *)&g_erasetest;
}
