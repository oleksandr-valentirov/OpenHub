# FreeRTOS: конфігурація задач (бекап перед міграцією CMSIS v1 → v2)

Записано з `git show e5c3bd9:testHubFreeRTOS.ioc` — останнього стану, у якому проект
збирався і працював. Потрібно, бо регенерація `.ioc` після міграції версії CubeMX
видалила секцію `FREERTOS_M7` цілком, а нова версія CubeMX підтримує лише CMSIS v2.

## 1. Що було (CMSIS v1)

Рядки з `.ioc`:

```
FREERTOS_M7.FootprintOK=true
FREERTOS_M7.IPParameters=Tasks01,configMINIMAL_STACK_SIZE,FootprintOK,configGENERATE_RUN_TIME_STATS,configUSE_STATS_FORMATTING_FUNCTIONS,configCHECK_FOR_STACK_OVERFLOW,Queues01
FREERTOS_M7.Queues01=cryptQueue,64,crypt_queue_element_t,0,Static,cryptQueueBuffer,cryptQueueControlBlock
FREERTOS_M7.Tasks01=defaultTask,0,512,StartDefaultTask,Default,NULL,Dynamic,NULL,NULL;cliTask,-3,512,CLI_Task,As external,NULL,Static,cliTaskBuffer,cliTaskControlBlock;cryptTask,2,512,Crypt_Task,As external,NULL,Static,cryptTaskBuffer,cryptTaskControlBlock
FREERTOS_M7.configCHECK_FOR_STACK_OVERFLOW=1
FREERTOS_M7.configGENERATE_RUN_TIME_STATS=1
FREERTOS_M7.configMINIMAL_STACK_SIZE=512
FREERTOS_M7.configUSE_STATS_FORMATTING_FUNCTIONS=1
Mcu.Pin37=VP_FREERTOS_M7_VS_CMSIS_V1
```

## 2. Задачі та черга

| Об'єкт | Entry function | Пріоритет v1 | Пріоритет v2 | Стек (слів) | Allocation | Buffer / Control block |
|---|---|---|---|---|---|---|
| `defaultTask` | `StartDefaultTask` | `osPriorityNormal` (0) | **24** | 512 | Dynamic | — |
| `cliTask` | `CLI_Task` (As external) | `osPriorityIdle` (−3) | **1** | 512 | Static | `cliTaskBuffer` / `cliTaskControlBlock` |
| `cryptTask` | `Crypt_Task` (As external) | `osPriorityHigh` (2) | **40** | 512 | Static | `cryptTaskBuffer` / `cryptTaskControlBlock` |

| Черга | Розмір | Тип елемента | Allocation | Buffer / Control block |
|---|---|---|---|---|
| `cryptQueue` | 64 | `crypt_queue_element_t` | Static | `cryptQueueBuffer` / `cryptQueueControlBlock` |

Параметри конфігурації: `configMINIMAL_STACK_SIZE` 512, `configCHECK_FOR_STACK_OVERFLOW` 1,
`configGENERATE_RUN_TIME_STATS` 1, `configUSE_STATS_FORMATTING_FUNCTIONS` 1.

### Таблиця пріоритетів v1 → v2

`osPriority_t` у `cmsis_os2.h` має інші числові значення, ніж `osPriority` у v1:

| Назва | v1 | v2 |
|---|---|---|
| `osPriorityIdle` | −3 | 1 |
| `osPriorityLow` | −2 | 8 |
| `osPriorityBelowNormal` | −1 | 16 |
| `osPriorityNormal` | 0 | 24 |
| `osPriorityAboveNormal` | 1 | 32 |
| `osPriorityHigh` | 2 | 40 |
| `osPriorityRealtime` | 3 | 48 |

## 3. Що фактично довелось змінити (міграція виконана)

Регенерація пройшла: CubeMX прийняв записи з розділу 1, згенерував усі три задачі
й чергу вже у v2 API (`osThreadNew`, `osMessageQueueNew`) — у `main.c`, не в
`freertos.c`, як я спершу очікував.

**3.1. Сигнатури задач** — `void const *` → `void *` у `cli.h`, `cli.c`,
`crypt.h`, `crypt.c`. Тип хендла черги — `osMessageQId` → `osMessageQueueId_t`.

**3.2. Заголовки FreeRTOS.** Головна несподіванка v2: `cmsis_os.h` більше **не**
підтягує сирий FreeRTOS API. Довелось додати явно:

| Файл | Що використовує | Додано |
|---|---|---|
| `cli.c` | `pdMS_TO_TICKS`, `vTaskList` | `FreeRTOS.h`, `task.h` |
| `crypt.c` | `xQueueSend/Receive`, `pdTRUE`, `errQUEUE_FULL` | `FreeRTOS.h`, `task.h`, `queue.h` |
| `networking.c` | `xTaskGetTickCount` | `FreeRTOS.h`, `task.h` |
| `newlib_stubs.c` | `pvPortMalloc`, `vPortFree`, `TaskHandle_t` | `FreeRTOS.h`, `task.h` |

**3.3. Прибрано з `main.c`** блоки, що дублювали згенероване: оголошення хендлів
у `USER CODE PV`, прототипи в `USER CODE PFP`, і v1-визначення `StartDefaultTask`
у `USER CODE 4`. Тіло (`HAL_HSEM_Release` + `HAL_HSEM_ActivateNotification`)
перенесено у `USER CODE 5` згенерованої задачі; `MX_LWIP_Init()` там уже був.

**3.4. `ethernetif.c`: `TxConfig`.** Новий шаблон використовує локальний
`tx_config` у `low_level_output`, але `low_level_init` усе ще налаштовує глобальний
`TxConfig`, якого шаблон більше не оголошує. Оголошення додано в `USER CODE 3`,
щоб пережило наступну регенерацію.

**3.5. `freertos.c`: `vApplicationStackOverflowHook`.** Прототип шаблону має
`char *`, а стара `__weak` заглушка в `USER CODE 4` — `signed char *`. Заглушку
видалено: реальна реалізація живе в `newlib_stubs.c` і збігається з прототипом.

**3.6. `osDelay` — без змін**, `configTICK_RATE_HZ = 1000`, тож тік = 1 мс.

## 4. Перевірка на залізі

Після перезбірки та прошивки прочитати через SWD:

```
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <addr uxCurrentNumberOfTasks> 0x8
```

Адреси беруться з `arm-none-eabi-nm CM7/build/testHubFreeRTOS_CM7.elf`.
Робочий стан після міграції: `uxCurrentNumberOfTasks` = **8** —
defaultTask, cliTask, cryptTask (з `main.c`), ethernet_link_thread (`lwip.c`),
ethernetif_input (`ethernetif.c`), tcpip_thread, IDLE, Tmr Svc.
`xTickCount` зростає, `pxCurrentTCB` не NULL.

Виміряно 2026-08-19 після прошивки обох ядер:
`uxCurrentNumberOfTasks` = 8, `xTickCount` 0x11F4 → 0x2836,
CM4 `rfm_ms_counter` = 0x286B.
