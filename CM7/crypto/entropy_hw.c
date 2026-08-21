/**
 * @file entropy_hw.c
 * @brief Feeds mbedTLS's entropy pool from the guarded hardware RNG.
 *
 * radio_devices_docs/open_hub/security/entropy.md
 */
#include <string.h>

#include "mbedtls/entropy.h"
#include "rng.h"

/* mbedTLS's hardware entropy hook, routed through the guarded RNG service.
 * radio_devices_docs/open_hub/security/entropy.md */
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
