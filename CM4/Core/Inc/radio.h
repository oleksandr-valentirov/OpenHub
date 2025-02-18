#pragma once

#include <stdint.h>

uint8_t RFM_Init(uint8_t network_id, uint8_t node_id);
void RFM_Routine(void);

typedef struct radio_header {
    uint8_t length;
    uint8_t addr;
} __attribute__((packed)) radio_header_t;

typedef struct radio_broadcast {
    uint32_t flags;
    uint32_t clock;
} __attribute__((packed)) radio_broadcast_t;