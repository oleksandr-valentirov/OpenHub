#pragma once

#include <stdint.h>

uint8_t RFM_Init(uint8_t network_id, uint8_t node_id);
void RFM_Routine(void);
