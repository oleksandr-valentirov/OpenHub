#include <string.h>

#include "mbedtls/entropy.h"
#include "rng.h"

/* mbedTLS's hardware entropy hook, enabled by MBEDTLS_ENTROPY_HARDWARE_ALT.
 *
 * Routed through the guarded RNG service rather than the HAL: on the H755 the
 * HAL cannot report a seed error at all, so HAL_RNG_GenerateRandomNumber would
 * hand back words that ST's own documentation says must not be used. A failure
 * here is reported, never silently substituted - seeding a DRBG with a wrong
 * value is worse than failing to seed it. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;

    if (output == NULL || olen == NULL)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *olen = 0;
    if (rng_bytes(output, len) != RNG_OK)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *olen = len;
    return 0;
}
