#include "random.h"
#include "main.h"  /* for RNG_HandleTypeDef */
#include "hsem_table.h"


static uint32_t seed = 0, m = 0, c = 0;

uint16_t Random_FromTS(uint32_t ts) {
    seed = (ts * seed + c) % m;
    return (seed >> 16);
}

void Random_SetSeed(uint32_t s) {
    seed = s;
}

uint8_t Random_Init(uint32_t s) {
    seed = s;

    while (HAL_HSEM_IsSemTaken(HSEM_RNG) && !((RNG->SR & RNG_SR_DRDY) != 0U)) {}
    HAL_HSEM_FastTake(HSEM_RNG);
    m = RNG->DR;
    while (!((RNG->SR & RNG_SR_DRDY) != 0U)) {}
    c = RNG->DR;
    HAL_HSEM_Release(HSEM_RNG, 0);

    return 0;
}
