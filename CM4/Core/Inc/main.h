/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include "stm32h7xx_nucleo.h"
#include <stdio.h>

#include "stm32h7xx_ll_rcc.h"
#include "stm32h7xx_ll_crs.h"
#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_system.h"
#include "stm32h7xx_ll_exti.h"
#include "stm32h7xx_ll_cortex.h"
#include "stm32h7xx_ll_utils.h"
#include "stm32h7xx_ll_pwr.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_rng.h"
#include "stm32h7xx_ll_tim.h"
#include "stm32h7xx_ll_usart.h"
#include "stm32h7xx_ll_gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stm32h7xx_hal_spi.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
void MX_RNG_Init(void);

/* USER CODE BEGIN EFP */
void rfm_write(uint8_t addr, uint8_t *ptr, uint8_t len);
void rfm_read(uint8_t addr, uint8_t *ptr, uint8_t len);
uint8_t get_delay_ms_flag(void);
void delay_ms_poll(uint32_t ms);
void delay_ms_it(uint32_t ms);
uint8_t get_delay_ms_flag(void);
uint32_t get_rfm_counter(void);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define RFM_DIO3_Pin LL_GPIO_PIN_8
#define RFM_DIO3_GPIO_Port GPIOC
#define RFM_DIO4_Pin LL_GPIO_PIN_9
#define RFM_DIO4_GPIO_Port GPIOC
#define RFM_CS_Pin LL_GPIO_PIN_15
#define RFM_CS_GPIO_Port GPIOA
#define RFM_DIO0_Pin LL_GPIO_PIN_12
#define RFM_DIO0_GPIO_Port GPIOC
#define RFM_DIO1_Pin LL_GPIO_PIN_2
#define RFM_DIO1_GPIO_Port GPIOD
#define RFM_DIO2_Pin LL_GPIO_PIN_10
#define RFM_DIO2_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
