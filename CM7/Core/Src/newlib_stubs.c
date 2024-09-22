#include "stm32h7xx_ll_usart.h"


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