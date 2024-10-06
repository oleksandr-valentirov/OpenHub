/* includes */
#include "cmsis_os.h"
#include "crypt.h"

/* variables */
extern CRYP_HandleTypeDef hcryp;
extern osMessageQId cryptQueueHandle;

/* function prototypes */
static inline HAL_StatusTypeDef Crypt_EncryptDecryptData(crypt_queue_element_t *data);


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
            if (data->crypt_op == CRYPT_OP_ENCRYPT)
                status = HAL_CRYP_Encrypt(&hcryp, data->input, data->data_len, data->output, 1000);
            else
                status = HAL_CRYP_Decrypt(&hcryp, data->input, data->data_len, data->output, 1000);
            break;
        default:
            status = HAL_ERROR;
            break;
    }

    if (status == HAL_OK && data->onSuccessCallback != NULL)
        data->onSuccessCallback();

    return status;
}
