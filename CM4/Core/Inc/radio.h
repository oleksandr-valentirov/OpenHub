#pragma once

#include <stdint.h>

uint8_t RFM_Init(uint8_t network_id, uint8_t node_id);
void RFM_Routine(void);

typedef struct rfm_header {
    uint8_t length;
} __attribute__((packed)) rfm_header_t;

typedef struct radio_broadcast {
    uint8_t addr;
    uint32_t hub_id;
    uint32_t flags;
    uint32_t clock;
} __attribute__((packed)) radio_broadcast_t;
