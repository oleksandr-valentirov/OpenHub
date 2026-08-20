#pragma once

#include <stddef.h>
#include <stdint.h>

/* Guarded access to the hardware RNG.
 *
 * The peripheral sits in D2 and both cores can reach it, so every draw is taken
 * under HSEM_RNG - two cores reading RNG_DR at once each get half a conditioned
 * word. Draws are also checked for a seed error, which the HAL does not do on
 * this part: RNG_RecoverSeedError is compiled out when the IP has no CONDRST,
 * and the H755 does not have it. A word drawn while SEIS is latched may carry
 * too little entropy, and nothing else in the system would notice. */

typedef enum {
    RNG_OK = 0,
    RNG_ERR_ARG,
    RNG_ERR_SEED,     /* the generator kept failing its entropy checks */
    RNG_ERR_CLOCK,    /* the kernel clock is wrong - check HSI48 */
    RNG_ERR_TIMEOUT   /* no word appeared, or the semaphore never came free */
} rng_status_t;

/* Clears the latched error flags, restarts the generator and flushes the words
 * drawn before it settled. Call once after MX_RNG_Init on the owning core. */
rng_status_t rng_init(void);

rng_status_t rng_word(uint32_t *out);
rng_status_t rng_bytes(void *dst, size_t len);

/* Last raw RNG_SR seen by a failing draw, for the console to print. */
uint32_t rng_last_status_reg(void);
