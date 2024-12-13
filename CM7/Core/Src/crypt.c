/* includes */
#include "cmsis_os.h"
#include "crypt.h"
#include "core_cm7.h"
#include <stdio.h>
#include <string.h>
#include "hsem_table.h"

/* variables */
extern CRYP_HandleTypeDef hcryp;
extern osMessageQId cryptQueueHandle;
static volatile uint8_t data_ready = 0;         /* for general encryption task */

/* CLI call variables */
static volatile uint8_t encryption_flag = 0;    /* for CLI encryption call */
__attribute__((section(".cli_dma_tx_buffer"))) static uint8_t dma_tx_buf[32] = {0};
__attribute__((section(".cli_dma_rx_buffer"))) static uint8_t dma_rx_buf[32] = {0};
/* ------------- */

/* function prototypes */
static inline HAL_StatusTypeDef Crypt_EncryptDecryptData(crypt_queue_element_t *data);


void HAL_CRYP_OutCpltCallback(CRYP_HandleTypeDef *hcryp) {
    UNUSED(hcryp);
    data_ready = 1;
}

void HAL_CRYP_ErrorCallback(CRYP_HandleTypeDef *hcryp) {
    UNUSED(hcryp);
    printf("\r\n%s - error\r\n", __func__);
}

void Crypt_Task(void const * argument) {
    UNUSED(argument);
    crypt_queue_element_t element;

    for (;;) {
        while (hcryp.State != HAL_CRYP_STATE_READY) {
            osDelay(1);
        }

        if (xQueueReceive(cryptQueueHandle, &element, 0) == pdTRUE)
            Crypt_EncryptDecryptData(&element);

        osDelay(1);
    }
}

static inline HAL_StatusTypeDef Crypt_EncryptDecryptData(crypt_queue_element_t *data) {
    HAL_StatusTypeDef status = HAL_OK;
    CRYP_ConfigTypeDef config = hcryp.Init;
    uint32_t start = 0;

    switch (hcryp.State) {
        case HAL_CRYP_STATE_BUSY:
            status = HAL_BUSY;
            break;
        case HAL_CRYP_STATE_RESET:
            status = HAL_ERROR;
            break;
        case HAL_CRYP_STATE_READY:
            if (data->key != NULL) {
                HAL_CRYP_GetConfig(&hcryp, &config);
                config.pKey = data->key;
                HAL_CRYP_SetConfig(&hcryp, &config);
            }

            if (data->data_len % 16 == 0) {
                data_ready = 0;
#if defined (__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
/* if dcache is disabled, clean or invalidation op can cause problems */
                SCB_CleanDCache_by_Addr(data->input, 32);
                SCB_InvalidateDCache_by_Addr(data->output, 32);
#endif
                if (data->crypt_op == CRYPT_OP_ENCRYPT)
                    status = HAL_CRYP_Encrypt_DMA(&hcryp, data->input, 4, data->output);
                else
                    status = HAL_CRYP_Decrypt_DMA(&hcryp, data->input, 4, data->output);
            }
            break;
        default:
            status = HAL_ERROR;
            break;
    }

    start = xTaskGetTickCount();
    while (xTaskGetTickCount() < (start + 1000)) {
        if (data_ready) {
#if defined (__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
/* if dcache is disabled, clean or invalidation op can cause problems */
            SCB_InvalidateDCache_by_Addr(data->output, 32);
#endif
            if (data->onSuccessCallback != NULL)
                data->onSuccessCallback();
            break;
        } else
            osDelay(1);
    }

    return status;
}

static void set_crypt_flag(void) {
    encryption_flag = 1;
}

int CRYPT_encrypt_data(char *data, char *resp_buffer) {
    uint32_t key[4] = {0};
    crypt_queue_element_t crypt_packet = {
        .input = ((uint32_t *)dma_tx_buf),
        .output = ((uint32_t *)dma_rx_buf),
        .crypt_op = CRYPT_OP_ENCRYPT, 
        .onSuccessCallback = &set_crypt_flag,
        .key = key
    };
    // HAL_StatusTypeDef status = HAL_OK;
    uint32_t start = 0;
    uint16_t len = 0;
    uint8_t i = 0;

    if (data == NULL)
        return sprintf(resp_buffer, "%s: No data\r\n", __func__);

    crypt_packet.data_len = (uint16_t)strlen(data);

    if (crypt_packet.data_len > 64)
        return sprintf(resp_buffer, "%s: text is longer than 64 bytes\r\n", __func__);
    
    /* copy data to DMA tx array */
    for (i = 0; i < crypt_packet.data_len; i++)
        dma_tx_buf[i] = data[i];

    /* generate AES-128 key */
    while (HAL_HSEM_IsSemTaken(HSEM_RNG)) {}
    HAL_HSEM_FastTake(HSEM_RNG);
    for (uint8_t i = 0; i < 4; i++) {
        while (!LL_RNG_IsActiveFlag_DRDY(RNG)) {}
        key[i] = LL_RNG_ReadRandData32(RNG);
        // return sprintf(resp_buffer, "\r\n%s: RNG error - %i\r\n", __func__, (int)status)
    }
    HAL_HSEM_Release(HSEM_RNG, 0);

    /* send data to the CRYP task */
    encryption_flag = 0;
    if (xQueueSend(cryptQueueHandle, (void *)&crypt_packet, 0) == errQUEUE_FULL)
        return sprintf(resp_buffer, "\r\n%s: Encryption error - crypt queue is full\r\n", __func__);

    start = xTaskGetTickCount();  /* wait till either the response is ready or the timeout */
    while (!encryption_flag && (xTaskGetTickCount() < (start + 1000)))
        osDelay(1);

    if (encryption_flag) {
        /* form a response to the CLI */
        len = sprintf(resp_buffer, "\r\nkey - 0x%08x 0x%08x 0x%08x 0x%08x\r\nencrypt>> ", (unsigned int)key[0], (unsigned int)key[1], (unsigned int)key[2], (unsigned int)key[3]);
        for (i = 0; i < crypt_packet.data_len; i++) {
            len += sprintf(resp_buffer + len, "0x%02x ", dma_rx_buf[i]);
        }

        /* prepare data for the decryption */
        for (i = 0; i < 32; i++)
            dma_tx_buf[i] = dma_rx_buf[i];

        encryption_flag = 0;
        crypt_packet.crypt_op = CRYPT_OP_DECRYPT;  /* send data to the CRYP task */
        if (xQueueSend(cryptQueueHandle, (void *)&crypt_packet, 0) == errQUEUE_FULL)
            return sprintf(resp_buffer, "\r\n%s: Decryption error - crypt queue is full\r\n", __func__);

        start = xTaskGetTickCount();  /* wait till either the response is ready or the timeout */
        while (!encryption_flag && (xTaskGetTickCount() < (start + 1000)))
            osDelay(1);

        if (encryption_flag) {  /* add decrypted data to the CLI response */
            len += sprintf(resp_buffer + len, "\r\ndecrypt>> ");
            for (i = 0; i < crypt_packet.data_len; i++)
                len += sprintf(resp_buffer + len, "%c", dma_rx_buf[i]);
            len += sprintf(resp_buffer + len, "\r\n");
        } else
            len += sprintf(resp_buffer + len, "\r\nDecryption timeout\r\n");

        return len;
    }

    return sprintf(resp_buffer, "\r\n%s: Encryption error - timeout\r\n", __func__);
}
