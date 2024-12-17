#pragma once

#include <stdint.h>

uint16_t Random_FromTS(uint32_t ts);
void Random_SetSeed(uint32_t s);
uint8_t Random_Init(uint32_t s);
