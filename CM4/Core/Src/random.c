#include "random.h"
#include "main.h"  /* for RNG_HandleTypeDef */


static uint32_t seed = 0, m = 0, c = 0;
extern RNG_HandleTypeDef hrng;

uint16_t Random_FromTS(uint32_t ts) {
    seed = (ts * seed + c) % m;
    return (seed >> 16);
}

void Random_SetSeed(uint32_t s) {
    seed = s;
}

uint8_t Random_Init(uint32_t s) {
    seed = s;

    while (hrng.State != HAL_RNG_STATE_READY) {}
    if (HAL_RNG_GenerateRandomNumber(&hrng, &m) != HAL_OK)
        return 1;

    while (hrng.State != HAL_RNG_STATE_READY) {}
    if (HAL_RNG_GenerateRandomNumber(&hrng, &c) != HAL_OK)
        return 1;

    return 0;
}
