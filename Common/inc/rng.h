#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @file rng.h
 * @brief Guarded access to the hardware RNG, under HSEM_RNG and seed-error checked.
 *
 * Both cores can reach RNG_DR, and two concurrent reads each take half a
 * conditioned word. radio_devices_docs/open_hub/security/entropy.md
 */

typedef enum {
    RNG_OK = 0,
    RNG_ERR_ARG,
    RNG_ERR_SEED,     /**< the generator kept failing its entropy checks */
    RNG_ERR_CLOCK,    /**< the kernel clock is wrong - check HSI48 */
    RNG_ERR_TIMEOUT   /**< no word appeared, or the semaphore never came free */
} rng_status_t;

/**
 * @brief Clears the latched flags and flushes the unsettled words.
 * @retval RNG_OK  the generator is usable
 * @return otherwise the status that stopped it
 *
 * Call once after MX_RNG_Init: SEIS stays latched across a reset and a draw
 * under it is not random. radio_devices_docs/open_hub/security/entropy.md
 */
rng_status_t rng_init(void);

/**
 * @brief Draws one conditioned word under HSEM_RNG.
 * @param out  receives the word, untouched on any failure
 * @retval RNG_OK  drawn
 * @return otherwise the reason, which is never folded into a single error
 */
rng_status_t rng_word(uint32_t *out);

/**
 * @brief Fills a buffer, drawing as many words as it takes.
 * @param dst  receives the bytes
 * @param len  how many
 * @retval RNG_OK  filled
 * @return otherwise the reason the first failing draw gave
 */
rng_status_t rng_bytes(void *dst, size_t len);

/**
 * @brief The raw RNG_SR a failing draw last saw.
 * @return the register, so the console prints the part's own answer
 */
uint32_t rng_last_status_reg(void);
