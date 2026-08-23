/**
 * @file cfgflash.h
 * @brief The flash side of the configuration store: erase, program, verify.
 *
 * ADR-0027 §7. The erase runs from ITCM and calls nothing, because the HAL lives
 * in the bank being erased. radio_devices_docs/open_hub/arch/config-store.md
 */
#ifndef CFGFLASH_H
#define CFGFLASH_H

#include <stdint.h>

#include "main.h"

/** Why an operation refused. Each names one condition; none shares a number. */
typedef enum {
    CFGF_OK = 0,
    CFGF_ERR_SECTOR,     /**< not one of the store's own sectors */
    CFGF_ERR_CM4_HELD,   /**< HSEM_ID_0 still taken: CM4's watchdog is armed */
    CFGF_ERR_SCHEDULER,  /**< the scheduler is running and this masks interrupts */
    CFGF_ERR_UNLOCK,     /**< the flash would not unlock */
    CFGF_ERR_ALIGN,      /**< address or length is not whole flash words */
    CFGF_ERR_RANGE,      /**< the write leaves the sector it started in */
    CFGF_ERR_NOT_ERASED, /**< the target is not virgin; overwriting corrupts ECC */
    CFGF_ERR_PROGRAM,    /**< the part reported an error bit */
    CFGF_ERR_VERIFY,     /**< it programmed and read back different */
    CFGF_ERR_NO_ITCM     /**< the erase routine did not reach ITCM */
} cfgflash_err_t;

/**
 * @brief Copies the ITCM-resident erase routine into ITCM. Call once, at boot.
 * @return bytes copied; zero means the section was discarded and no erase may run
 *
 * `.itcmfunc` has a load address in flash and no startup loop of its own, and it
 * is reached by address rather than through the call graph - so the linker script
 * KEEPs it. radio_devices_docs/open_hub/arch/config-store.md
 */
uint32_t cfgflash_init(void);

/**
 * @brief Erases one of the store's sectors, from ITCM, with interrupts masked.
 * @param sector    CFG_IDENTITY_SECTOR, CFG_JOURNAL_SECTOR_A or _B
 * @param ms_out    receives how long it took, or NULL
 * @return CFGF_OK, or why it refused
 *
 * Takes about 954 ms and stalls nothing on CM4, which runs from bank 2.
 */
cfgflash_err_t cfgflash_erase(uint8_t sector, uint32_t *ms_out);

/**
 * @brief Programs whole flash words into erased space and reads them back.
 * @param addr   destination, 32-byte aligned, inside one of the store's sectors
 * @param src    source bytes
 * @param bytes  length, a multiple of 32
 * @return CFGF_OK, or why it refused
 */
cfgflash_err_t cfgflash_program(uint32_t addr, const void *src, uint32_t bytes);

/**
 * @brief Whether a span reads as never programmed.
 * @param addr   start
 * @param bytes  length
 * @retval 1  every byte is 0xFF
 * @retval 0  something is there
 */
int cfgflash_is_erased(uint32_t addr, uint32_t bytes);

/**
 * @brief The base address of one of the store's sectors.
 * @param sector  the sector number
 * @return its address, or 0 if it is not one of ours
 */
uint32_t cfgflash_sector_addr(uint8_t sector);

/** @brief The reason as a word for a console line. @return a static string, never NULL */
const char *cfgflash_err_str(cfgflash_err_t e);

/** One bit per guard that failed to refuse what it exists to refuse. */
enum {
    CFGF_ST_ITCM       = 1u << 0,  /**< the erase routine is not in ITCM */
    CFGF_ST_SECTOR     = 1u << 1,  /**< a sector that is not ours was accepted */
    CFGF_ST_SCHEDULER  = 1u << 2,  /**< an erase was accepted with the scheduler up */
    CFGF_ST_ALIGN      = 1u << 3,  /**< an unaligned or part-word write was accepted */
    CFGF_ST_RANGE      = 1u << 4,  /**< a write outside the store's sectors was accepted */
    CFGF_ST_NOT_ERASED = 1u << 5,  /**< a write over live data was accepted */
    CFGF_ST_STRINGS    = 1u << 6,  /**< two reasons render as one word */
    /* A check whose population can be empty must fail on empty.
     * radio_devices_docs/open_hub/arch/config-store.md */
    CFGF_ST_NO_POP     = 1u << 7   /**< the overwrite guard had nothing to overwrite */
};

/**
 * @brief Exercises every refusal path. Touches no flash and erases nothing.
 * @return 0 when every guard refused; otherwise a mask of CFGF_ST_* that did not
 *
 * A guard that has never refused anything is indistinguishable from one that
 * cannot. radio_devices_docs/open_hub/arch/config-store.md
 */
uint32_t cfgflash_selftest(void);

/**
 * The erase, as one instruction sequence two differently-placed functions can
 * share. The 954 ms measurement describes this body, so a driver that ran a
 * different one would leave that number describing nothing.
 * radio_devices_docs/open_hub/arch/config-store.md
 */
#define CFGFLASH_ERASE_BODY(sector, spins)                                    \
    uint32_t sr;                                                              \
    FLASH->CCR1 = 0xFFFFFFFFu;                                                \
    if ((FLASH->CR1 & FLASH_CR_LOCK) != 0u) {                                 \
        FLASH->KEYR1 = 0x45670123u;                                           \
        FLASH->KEYR1 = 0xCDEF89ABu;                                           \
    }                                                                         \
    FLASH->CR1 &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);                           \
    FLASH->CR1 |= (FLASH_CR_SER | FLASH_VOLTAGE_RANGE_3 |                     \
                   ((uint32_t)(sector) << FLASH_CR_SNB_Pos) | FLASH_CR_START);\
    __DSB();                                                                  \
    while ((FLASH->SR1 & (FLASH_SR_QW | FLASH_SR_BSY)) != 0u)                 \
        (*(spins))++;                                                         \
    sr = FLASH->SR1;                                                          \
    FLASH->CR1 &= ~FLASH_CR_SER;                                              \
    return sr

#endif /* CFGFLASH_H */
