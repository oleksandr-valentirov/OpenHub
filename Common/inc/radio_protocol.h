#pragma once

#include <stdint.h>


typedef struct protocol_header {
    uint32_t hub_id;
    uint32_t dev_id;
} __attribute__((packed)) protocol_header_t;

typedef struct radio_pairing {
    protocol_header_t header;
    uint8_t stage;
} __attribute__((packed)) protocol_pairing_t;
