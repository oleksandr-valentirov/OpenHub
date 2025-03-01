#pragma once

#include <stdint.h>


typedef enum {
    RFM_READ_REG = 0,
    RFM_ADD_DEVICE,
    RFM_REMOVE_DEVICE,
    RFM_GET_DEVICE_INFO,
    RFM_SET_DEVICE_PARAM
} rfm_request_t;

typedef struct m7_to_m4_rfm_request
{
    uint8_t request_type;   /* 0 - to RFM reg */
    uint8_t arg;            /* can be RFM address or dev ID */
    uint8_t len;
    uint8_t payload[61];
} m7_to_m4_rfm_request_t;
