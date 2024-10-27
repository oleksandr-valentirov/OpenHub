#include "stm32h7xx_ll_usart.h"
#include "cmsis_os.h"


int _read(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while (!LL_USART_IsActiveFlag_RXNE_RXFNE(UART4)) {}
        *(ptr++) = (char)LL_USART_ReceiveData8(UART4);
    }

    return len;
}

int _write(int file, char *ptr, int len) {
    UNUSED(file);

    for (int i = 0; i < len; i++) {
        while (!(LL_USART_IsActiveFlag_TC(UART4) || LL_USART_IsActiveFlag_TXE_TXFNF(UART4))) {}
        LL_USART_TransmitData8(UART4, *(ptr++));
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
