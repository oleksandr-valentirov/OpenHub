# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Conventions

- Code comments in English.
- A comment block is at most 100 characters. One line, not a paragraph.
- Comments state why, not what. No restating the code, no filler.
- Committing is fine. Do **not** add `Co-Authored-By` or any co-authorship trailer.

## Build

```bash
cmake --preset Debug          # configures both cores
cmake --build --preset Debug  # -> CM4/build/*.elf, CM7/build/*.elf
```

Presets: `Debug`, `RelWithDebInfo`, `Release`, `MinSizeRel`. The top-level project drives each
core through `ExternalProject_Add`, so a single core is built by configuring its directory:

```bash
cmake -S CM7 -B CM7/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/gcc-arm-none-eabi.cmake && cmake --build CM7/build
```

There are no tests. Toolchain comes from STM32CubeCLT (`arm-none-eabi-gcc`, `cmake`, `ninja`).

## Flash and inspect

CM7 lives at `0x08000000`, CM4 at `0x08100000`; writing the ELF picks the address up automatically.

```bash
P=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -w CM7/build/testHubFreeRTOS_CM7.elf -v
$P -c port=SWD mode=UR -w CM4/build/testHubFreeRTOS_CM4.elf -v
$P -c port=SWD mode=HOTPLUG --rst
```

To check the firmware actually runs, read RTOS state over SWD instead of guessing. Get addresses
with `arm-none-eabi-nm`, then `$P -c port=SWD mode=HOTPLUG -r32 <addr> 0x8`. Healthy CM7:
`uxCurrentNumberOfTasks` = 8, `xTickCount` rising, `pxCurrentTCB` non-NULL. On CM4,
`rfm_ms_counter` rising means the TIM7 interrupt is alive.

## Architecture

Dual-core STM32H755 on NUCLEO-H755ZI-Q. The cores split by responsibility and talk over shared
memory guarded by hardware semaphores.

**CM7** — application core. FreeRTOS with CMSIS-RTOS v2, LwIP in RTOS mode (`WITH_RTOS 1`).
Three application threads created in `main.c`: `defaultTask` (brings LwIP up, then releases CM4),
`cliTask` (UART4 command shell), `cryptTask` (AES-128 via the CRYP peripheral, fed by
`cryptQueue`). LwIP adds `tcpip_thread`, `ethernetif_input` and `ethernet_link_thread`.

**CM4** — radio core. No RTOS; a superloop calling `RFM_Routine()`. Drives an RFM69 over SPI1
through the `CM4/rfm69_lib` submodule. TIM7 provides the millisecond counter used for radio timing.

**Boot order matters.** CM4 enters STOP at reset and waits on `HSEM_ID_0`. CM7 holds that semaphore
across clock and peripheral init, then releases it from `StartDefaultTask` once LwIP is up. Moving
`MX_LWIP_Init()` back into `main()` breaks this — it must run after the scheduler starts.

**Cross-core IPC.** `Common/inc/hsem_table.h` assigns the semaphores; `Common/inc/shared_memory.h`
defines the request struct. CM7 writes it at a hardcoded `0x38000000` (SRAM4) and signals
`HSEM_M7_TO_M4_RFM`; CM4 answers via `HSEM_M4_TO_M7`. Nothing reserves that address in the linker
scripts, and the address literal is duplicated in `cli.c` and `radio.c`.

**Memory.** `.lwip_sec` occupies ~282 KB at `0x30000000`, which is essentially all of RAM_D2 (95%).
Adding LwIP buffers or threads will overflow it.

## CubeMX regeneration

The `.ioc` is the source of truth and regeneration overwrites a lot. Know what survives before
editing anything.

- **Generated, will be overwritten:** `CM4/mx-generated.cmake`, `CM7/mx-generated.cmake`, and
  everything in `main.c` / `freertos.c` / `ethernetif.c` / `lwip.c` outside `USER CODE` markers.
- **Generated once, safe to edit:** `CM4/CMakeLists.txt`, `CM7/CMakeLists.txt`. Project-level
  build decisions belong here.
- Put anything hand-written inside `USER CODE BEGIN/END` or it will be lost.

**Libraries are not vendored.** `ProjectManager.LibraryCopy=2` means CubeMX references HAL, CMSIS
and Middlewares from the Cube FW package and bakes absolute paths into `mx-generated.cmake`.
`CM4/CMakeLists.txt` and `CM7/CMakeLists.txt` rewrite that prefix to `${CUBE_FW_PATH}` at configure
time and include the processed copy from the build directory. The regex is independent of user and
FW version, so a committed file carrying someone else's absolute paths still builds. Override the
location with `-DCUBE_FW_PATH=<path>` or the `CUBE_FW_PATH` environment variable. There is no
post-generation hook; do not reintroduce one.

**Linker scripts.** The build links `custom_m4_flash.ld` / `custom_m7_flash.ld` via
`STM32_LINKER_SCRIPT`, not the `stm32h755xx_*.ld` files CubeMX writes. Custom sections
(`.lwip_sec`, `.cli_dma_buffer`) live in the custom files; edit those.

**Known template gaps** to re-apply if regeneration drops them:

- `cmsis_os.h` under CMSIS-RTOS v2 does not pull in the raw FreeRTOS API. Files using
  `xQueueSend`, `pdMS_TO_TICKS`, `pvPortMalloc` etc. need explicit `FreeRTOS.h` / `task.h` /
  `queue.h`.
- `ethernetif.c` still configures a global `TxConfig` in `low_level_init` that the current template
  no longer declares; the declaration is kept in `USER CODE 3`.

`docs/freertos-config-backup.md` records the FreeRTOS task and queue configuration plus the
CMSIS v1 to v2 migration, in case the `.ioc` loses it again.

## Known defects

Confirmed by type and control flow, not yet fixed:

- `radio.c` `RFM_send_broadcast` offsets the payload by `sizeof(header)` where `header` is a
  pointer, giving 4 instead of `sizeof(rfm_header_t)` = 1. Broadcast packets are malformed.
- `cli.c` `cfg` with no argument passes NULL to `strncmp`.
- `cli.c` passes three `strtok` calls as separate arguments to `set_server_ip_addr`; C leaves the
  evaluation order unspecified.
- `random.c` takes `% m` where `m` comes from the RNG and may be zero.
- Tick waits use exact equality (`HAL_GetTick() != end`), which hangs for ~49 days on a missed tick.
- Radio DIO polling loops have no timeout.
