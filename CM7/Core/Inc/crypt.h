#pragma once

#include "main.h"


typedef enum {
    CRYPT_OP_ENCRYPT = 0,
    CRYPT_OP_DECRYPT,
    CRYPT_OP_END
} crypt_op_t;

typedef struct crypt_queue_element {
    uint32_t    *input;
    uint32_t    *output;
    uint32_t    *key;
    crypt_op_t  crypt_op;
    uint16_t    data_len;
    void (*onSuccessCallback)(void);
} crypt_queue_element_t;

void Crypt_Task(void const * argument);
void HAL_CRYP_OutCpltCallback(CRYP_HandleTypeDef *hcryp);
void HAL_CRYP_ErrorCallback(CRYP_HandleTypeDef *hcryp);
