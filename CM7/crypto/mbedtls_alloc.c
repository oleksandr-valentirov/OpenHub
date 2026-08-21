/**
 * @file mbedtls_alloc.c
 * @brief mbedTLS's allocator, routed to the FreeRTOS heap rather than newlib's.
 *
 * radio_devices_docs/open_hub/security/crypto-architecture.md
 */
#include <string.h>

#include "FreeRTOS.h"
#include "portable.h"

/* Both halves pinned to the FreeRTOS heap: newlib's calloc never sees the
 * malloc override. radio_devices_docs/open_hub/security/crypto-architecture.md */

void *openhub_calloc(size_t n, size_t size) {
    size_t total;
    void *p;

    if (n != 0u && size > (size_t)-1 / n)
        return NULL;                  /* the multiply would wrap */

    total = n * size;
    p = pvPortMalloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

void openhub_free(void *p) {
    vPortFree(p);
}
