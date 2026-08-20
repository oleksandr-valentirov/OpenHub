#include <string.h>

#include "rng.h"
#include "hsem_table.h"
#include "stm32h7xx_hal.h"

/* Bounded rather than timed: this runs before the scheduler on CM7 and with no
 * scheduler at all on CM4, so HAL_GetTick is not dependable in either place. */
#define RNG_SPIN_LIMIT      100000u
/* Drawn and thrown away after a restart, while the conditioning settles. */
#define RNG_DISCARD_WORDS   8u
/* Depth of the output buffer: DRDY stays high until four words are read out. */
#define RNG_FIFO_WORDS      4u
#define RNG_SEED_RETRIES    4u

static uint32_t last_sr = 0;

static int rng_lock(void) {
    uint32_t spin = RNG_SPIN_LIMIT;

    while (HAL_HSEM_FastTake(HSEM_RNG) != HAL_OK) {
        if (--spin == 0u)
            return 0;
    }
    return 1;
}

static void rng_unlock(void) {
    HAL_HSEM_Release(HSEM_RNG, 0);
}

static void rng_clear_flags(void) {
    RNG->SR &= ~(RNG_SR_SEIS | RNG_SR_CEIS);
}

static rng_status_t rng_wait_ready(void) {
    uint32_t spin = RNG_SPIN_LIMIT;

    for (;;) {
        uint32_t sr = RNG->SR;

        if ((sr & RNG_SR_CECS) != 0u) {
            last_sr = sr;
            return RNG_ERR_CLOCK;
        }
        if ((sr & RNG_SR_DRDY) != 0u)
            return RNG_OK;
        if (--spin == 0u) {
            last_sr = sr;
            return RNG_ERR_TIMEOUT;
        }
    }
}

/* ST's documented recovery for an RNG without CONDRST: clear the latched flag,
 * then stop and start the generator so the seed is drawn again. The words right
 * after a restart are the ones that trip the entropy checks, so they are drawn
 * and dropped without being judged - refusing them here is how the first
 * attempt at this left SEIS latched forever. */
static void rng_restart_locked(void) {
    rng_clear_flags();
    RNG->CR &= ~RNG_CR_RNGEN;
    RNG->CR |= RNG_CR_RNGEN;

    for (uint32_t i = 0; i < RNG_DISCARD_WORDS; i++) {
        if (rng_wait_ready() != RNG_OK)
            break;
        (void)RNG->DR;
    }
    rng_clear_flags();
}

/* A word is trustworthy only if no seed error latched between clearing the flag
 * and reading the register, so the flag is cleared immediately before the draw
 * and tested immediately after it. Measured on this part, SEIS latches on its
 * own within about a second of idling, which is why it is cleared per draw
 * rather than once at startup. */
static rng_status_t rng_draw_locked(uint32_t *out) {
    rng_status_t st;
    uint32_t sr;

    /* Whatever is already buffered was produced before the flag was cleared, so
     * it cannot be vouched for by the check below and is dropped. */
    for (uint32_t i = 0; i < RNG_FIFO_WORDS; i++) {
        if ((RNG->SR & RNG_SR_DRDY) == 0u)
            break;
        (void)RNG->DR;
    }

    rng_clear_flags();

    st = rng_wait_ready();
    if (st != RNG_OK)
        return st;

    *out = RNG->DR;

    sr = RNG->SR;
    if ((sr & (RNG_SR_SEIS | RNG_SR_SECS)) != 0u) {
        last_sr = sr;
        return RNG_ERR_SEED;
    }
    return RNG_OK;
}

rng_status_t rng_init(void) {
    rng_status_t st = RNG_ERR_SEED;

    if (!rng_lock())
        return RNG_ERR_TIMEOUT;

    /* SEIS comes up latched out of reset on this part even when SECS has
     * already cleared itself, and HAL_RNG_Init never touches SR. */
    for (uint32_t attempt = 0; attempt < RNG_SEED_RETRIES; attempt++) {
        uint32_t w;

        rng_restart_locked();
        st = rng_draw_locked(&w);
        if (st == RNG_OK || st == RNG_ERR_CLOCK)
            break;
    }

    rng_unlock();
    return st;
}

rng_status_t rng_word(uint32_t *out) {
    rng_status_t st = RNG_ERR_SEED;

    if (out == NULL)
        return RNG_ERR_ARG;

    if (!rng_lock())
        return RNG_ERR_TIMEOUT;

    for (uint32_t attempt = 0; attempt < RNG_SEED_RETRIES; attempt++) {
        st = rng_draw_locked(out);
        if (st != RNG_ERR_SEED)
            break;
        rng_restart_locked();
    }

    rng_unlock();
    return st;
}

/* On failure the destination is zeroed, not left as it stands.
 *
 * A partial fill is the dangerous outcome: a caller that misses the return
 * value gets a buffer that is part fresh entropy and part whatever was there
 * before, and nothing downstream can tell. For a long-term private key that is
 * unrecoverable and silent. Zeroing makes a missed check produce an obviously
 * broken value instead of a plausible one - the same reason a refusal has to be
 * acted on rather than merely returned. */
rng_status_t rng_bytes(void *dst, size_t len) {
    uint8_t *p = (uint8_t *)dst;
    size_t remaining = len;

    if (dst == NULL)
        return RNG_ERR_ARG;

    while (remaining > 0u) {
        size_t n = (remaining < sizeof(uint32_t)) ? remaining : sizeof(uint32_t);
        uint32_t w;
        rng_status_t st = rng_word(&w);

        if (st != RNG_OK) {
            memset(dst, 0, len);
            return st;
        }

        memcpy(p, &w, n);
        p += n;
        remaining -= n;
    }
    return RNG_OK;
}

uint32_t rng_last_status_reg(void) {
    return last_sr;
}
