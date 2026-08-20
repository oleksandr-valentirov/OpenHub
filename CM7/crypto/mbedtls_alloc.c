#include <string.h>

#include "FreeRTOS.h"
#include "portable.h"

/* mbedTLS allocates with calloc and frees with free.
 *
 * newlib_stubs.c overrides malloc and free to reach the FreeRTOS heap, but not
 * calloc - newlib's calloc calls _malloc_r and never sees the override. mbedTLS
 * therefore allocated from the newlib heap and freed into the FreeRTOS one,
 * where heap_4's configASSERT fired on the foreign block header. With
 * configASSERT defined as taskDISABLE_INTERRUPTS() plus an empty loop, that
 * presents as a live core with a frozen tick and no fault recorded.
 *
 * Both halves are pinned to one heap here rather than relying on the overrides. */

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
