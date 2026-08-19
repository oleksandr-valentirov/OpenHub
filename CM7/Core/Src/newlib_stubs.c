#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>


int _read(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while ((UART4->ISR & USART_ISR_RXNE_RXFNE) == 0U) {}
        *(ptr++) = (char)(UART4->RDR & 0xFFU);
    }

    return len;
}

int _write(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while ((UART4->ISR & (USART_ISR_TC | USART_ISR_TXE_TXFNF)) == 0U) {}
        UART4->TDR = (uint8_t)*(ptr++);
    }

    return len;
}

int getchar(void) {
    int c = 0;
    _read(0, (char *)(&c), 1);
    return c;
}

/* 
 * Defining malloc/free should overwrite the standard versions provided by the compiler.
 * https://community.st.com/t5/stm32-mcus-embedded-software/lwip-rand-uses-newlib-rand-and-fails/m-p/720026/highlight/true#M51347
 */
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
