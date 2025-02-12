#pragma once

#include "rfm69_registers.h"
#include "main.h"   /* for pin defines */

/* portability defines */
extern SPI_HandleTypeDef hspi1;
#define RFM_SPI     (&hspi1)

#ifndef RFM_CS_GPIO_Port
#error "RFM_CS_GPIO_Port not defined"
#endif

#ifndef RFM_CS_Pin
#error "RFM_CS_Pin not defined"
#endif

#ifndef RFM_CS_Pin
#error "RFM_CS_Pin not defined"
#endif

#ifndef RFM_DIO0_GPIO_Port
#error "RFM_DIO0_GPIO_Port not defined"
#endif
#ifndef RFM_DIO0_Pin
#error "RFM_DIO0_Pin not defined"
#endif

#ifndef RFM_DIO1_GPIO_Port
#error "RFM_DIO1_GPIO_Port not defined"
#endif
#ifndef RFM_DIO1_Pin
#error "RFM_DIO1_Pin not defined"
#endif

#ifndef RFM_DIO2_GPIO_Port
#error "RFM_DIO2_GPIO_Port not defined"
#endif
#ifndef RFM_DIO2_Pin
#error "RFM_DIO2_Pin not defined"
#endif

#ifndef RFM_DIO3_GPIO_Port
#error "RFM_DIO3_GPIO_Port not defined"
#endif
#ifndef RFM_DIO3_Pin
#error "RFM_DIO3_Pin not defined"
#endif

#ifndef RFM_DIO4_GPIO_Port
#error "RFM_DIO4_GPIO_Port not defined"
#endif
#ifndef RFM_DIO4_Pin
#error "RFM_DIO4_Pin not defined"
#endif
/* portability defines end */

uint8_t RFM69_Init(uint8_t network_id, uint8_t node_id);
void RFM69_Routine(void);
