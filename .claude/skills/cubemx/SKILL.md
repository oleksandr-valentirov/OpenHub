---
name: cubemx
description: Regenerate this project's STM32 code from testHubFreeRTOS.ioc using headless STM32CubeMX. Use when peripherals, pins, clocks, FreeRTOS tasks or middleware need to change, or when a change is about to be hand-written into a generated file.
---

# CubeMX without the GUI

## The rule

**Anything CubeMX can generate must be generated.** Before hand-editing a file,
check whether the `.ioc` owns it. If it does, change the `.ioc` and regenerate —
a hand edit outside `USER CODE` markers is lost on the next run, and worse, it
hides the real configuration from the next person who opens the GUI.

Hand-written code belongs in `USER CODE BEGIN/END` blocks, in files CubeMX never
touches (`cli.c`, `radio.c`, the linker scripts), or in `CM4/CMakeLists.txt` /
`CM7/CMakeLists.txt`, which are generated once and then owned by the project.

## Running it

```bash
S=/absolute/path/to/script.txt
printf 'config load /home/aleks/Documents/repos/OpenHub/testHubFreeRTOS.ioc\nproject generate\nexit\n' > $S
cd /home/aleks/STM32CubeMX && xvfb-run -a ./STM32CubeMX -q $S
```

Three things bite here:

- **The script path must be absolute.** CubeMX resolves it relative to its own
  install directory, not the working directory. A relative path fails with
  `FileNotFoundException` and then the process sits there forever instead of
  exiting.
- **`xvfb-run`** keeps it off the user's display. It still builds a GUI, it just
  needs somewhere to put it. A plain `DISPLAY=` also works but pops windows.
- **Startup is slow** — allow 2-3 minutes for load plus generate. Run it in the
  background rather than blocking on a short timeout.

Other useful script commands: `config save`, `project name <n>`,
`project toolchain <t>`, `csv pinout <file>`.

## Always do a control run first

Before making any `.ioc` change, generate from the committed `.ioc` and confirm
`git status` is clean. That proves the toolchain reproduces the current state
byte for byte, so any diff after the real change is attributable to the change.
If the control run is dirty, fix that before going further — otherwise the real
diff is unreadable.

After the real run, read the whole diff. Confirm every `USER CODE` block that
mattered is still there, then build and flash before committing.

## Editing the .ioc

The file is `key=value`, LF endings, order matters only within a value. Edit it
with a script that preserves line endings, not with an editor that might rewrite
them.

Patterns worth knowing:

- **`<IP>.IPParameters`** lists which of that IP's keys are live. Removing a
  parameter means removing it from this list *and* deleting its own line.
- **Core assignment** is `CortexM4.IPs=` / `CortexM7.IPs=`. A `\:I` suffix means
  the IP is listed but not owned by that core. Moving a peripheral between cores
  means editing both lists.
- **`ProjectManager.functionlistsort`** decides which core's `main.c` gets the
  `MX_<IP>_Init()` call, and in what order. It must agree with the `IPs` lists.
- **`FREERTOS_M7.Tasks01`** is `;`-separated tasks, `Queues01` likewise. Dropping
  the last queue also means dropping `Queues01` from `IPParameters`.
- **DMA requests** are `Dma.<NAME>.<n>.*` plus `Dma.Request<n>=` and
  `Dma.RequestsNb=`. Removing one means renumbering the rest.

CubeMX fills in defaults for anything it needs and you left out — changing
`CRYP.Algorithm` to `CRYP_AES_GCM` is enough, it adds the IV and header fields
on its own. Let it, then read the diff.

## Project-specific traps

These are documented in full in `CLAUDE.md`; the short version:

- **`ProjectManager.LibraryCopy=2`** keeps HAL and middleware out of the repo and
  bakes absolute paths into `mx-generated.cmake`. The `CMakeLists.txt` files
  rewrite that prefix at configure time. Do not switch this to copy mode.
- **Linker scripts** are `custom_m4_flash.ld` / `custom_m7_flash.ld`, not the
  `stm32h755xx_*.ld` CubeMX writes. Custom sections live in the custom files.
- **CMSIS-RTOS v2 `stack_size` is bytes**, but the LwIP glue passes word counts
  straight through. Re-check the `* 4` in `ethernetif.c` after every generation.
- **`cmsis_os.h` under v2 does not pull in the raw FreeRTOS API** — files using
  `xQueueSend`, `pdMS_TO_TICKS` and friends need explicit includes.
- **`ethernetif.c`** keeps a `TxConfig` declaration in `USER CODE 3` that the
  current template no longer emits.
- **`.RxDescripSection` / `.TxDescripSection`** — the spelling in
  `custom_m7_flash.ld` must match what `ethernetif.c` emits. The widely-copied
  ST community article spells them without the `s`.

## Verification

```bash
cmake --build --preset Debug
P=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -w CM7/build/testHubFreeRTOS_CM7.elf -v
$P -c port=SWD mode=UR -w CM4/build/testHubFreeRTOS_CM4.elf -v
$P -c port=SWD mode=HOTPLUG --rst
```

Then read the console on `/dev/ttyACM0` at 115200: `?` lists commands, `status`
shows the task table and stack headroom, `rfm dump 10` returns `0x24` if CM4 and
the cross-core IPC are both alive.
