/* includes */
#include "cmsis_os.h"
#include "crypt.h"
#include <stdio.h>
#include "core_cm7.h"

/* variables */
extern CRYP_HandleTypeDef hcryp;
extern osMessageQId cryptQueueHandle;
static volatile uint8_t data_ready = 0;

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
