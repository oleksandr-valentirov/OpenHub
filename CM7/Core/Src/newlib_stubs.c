/**
 * @file newlib_stubs.c
 * @brief Newlib's I/O hooks, kept so nothing links against a missing symbol.
 *
 * The console does not go through stdio: _read polls the same peripheral
 */
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>


int _read(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while ((USART3->ISR & USART_ISR_RXNE_RXFNE) == 0U) {}
        *(ptr++) = (char)(USART3->RDR & 0xFFU);
    }

    return len;
}

int _write(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while ((USART3->ISR & (USART_ISR_TC | USART_ISR_TXE_TXFNF)) == 0U) {}
        USART3->TDR = (uint8_t)*(ptr++);
    }

    return len;
}

int getchar(void) {
    int c = 0;
    _read(0, (char *)(&c), 1);
    return c;
}

/* Defining malloc/free overrides the compiler's, but not calloc.
 * radio_devices_docs/open_hub/security/crypto-architecture.md */
void *malloc(size_t size) {
    /* Call the FreeRTOS version of malloc. */
    return pvPortMalloc(size);
}

void free(void *ptr) {
    /* Call the FreeRTOS version of free. */
    vPortFree(ptr);
}


void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName ) {
    (void) xTask;
    printf("overflow - %s\r\n", pcTaskName);
}
