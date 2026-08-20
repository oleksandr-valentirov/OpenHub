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

### Removal is automatic, addition is not

`project generate` rewrites the `.ioc` as a side effect, but only in one
direction. Setting `Dma.RequestsNb=0` was enough for it to drop `DMA` from
`Mcu.IP*`, renumber every entry after it and fix `Mcu.IPNb` by itself. Adding an
IP gets no such help: anything CubeMX does not recognise as fully declared is
**silently deleted from the `.ioc` on the next generate**, with nothing in the
log. If a new peripheral does not appear in `main.c`, look at the `.ioc` first —
your lines are probably gone.

To add a peripheral, all of these must be present:

| Key | Note |
|---|---|
| `Mcu.IP<n>=<IP>` | list is alphabetical; renumber from 0 |
| `Mcu.IPNb` | one **greater** than the number of `Mcu.IP<n>` lines |
| `Mcu.Pin<n>=VP_<inst>_VS_<sig>` | needed even for peripherals with no pins, **and it must sit in CubeMX's own order, not appended** |
| `Mcu.PinsNb` | exact count of `Mcu.Pin<n>` lines |
| `<IP>.IPParameters` + one line per parameter | |
| `VP_<inst>_VS_<sig>.Mode` / `.Signal` | the activation record — see below |
| `ProjectManager.functionlistsort` | adds the `MX_<IP>_Init()` call |
| `CortexM<n>.IPs` | append the name |

**The `VP_` entry is what actually activates an IP.** TIM2 and IWDG2 were added
in the same edit with everything else identical; TIM2 generated and IWDG2 did
not, because only TIM2 had its virtual-pin record. Get the exact names from the
IP's own modes file:

```bash
grep -o 'InstanceName="IWDG2" Name="[^"]*"' db/mcu/STM32H755ZITx.xml   # -> IWDG
grep -oE '<(Mode|RefSignal) Name="[^"]*"' db/mcu/IP/IWDG-iwdg1_v1_1_Modes.xml
```

`RefSignal Name="VS_IWDG"` and `Mode Name="IWDG_Activate"` then give
`VP_IWDG2_VS_IWDG.Mode=IWDG_Activate` and
`VP_IWDG2_VS_IWDG.Signal=IWDG2_VS_IWDG` — signal is prefixed with the
*instance*, mode is not.

Defaults still come for free: changing `CRYP.Algorithm` to `CRYP_AES_GCM` is
enough, CubeMX adds the IV and header fields on its own.

**Position in `Mcu.Pin<n>` is load-bearing.** The `VP_` block is roughly
alphabetical with the board's BSP entry pinned last, and CubeMX rejects an entry
appended out of place — silently, as above. Adding `VP_IWDG2_VS_IWDG` at the end
was dropped every time; at its sorted slot, with everything after it renumbered,
it took. A scripted insert must renumber the whole tail.

So: **use the GUI to add a peripheral, and scripts to change one that already
exists.** Changing values, moving an IP between cores, removing things and
editing NVIC or FreeRTOS entries are all reliable from a script. Introducing a
new IP means seven interlocking keys, and the ordering rule above is not
something the file advertises.

Verify anchors before inserting. Several keys you might reach for as insertion
points (`UART4.`, `TIM7.`) disappear once their IP is removed, and a
`str.replace` on a missing anchor fails silently.

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
- **Shared EXTI vectors get one pin each.** For a vector covering several lines
  CubeMX emits a single `HAL_GPIO_EXTI_IRQHandler(...)` call, so the other lines
  never have their flag cleared and the handler re-enters forever. CM4 keeps the
  extra calls for DIO0 and DIO4 in `USER CODE EXTI15_10_IRQn 1` and
  `USER CODE EXTI9_5_IRQn 1`.
- **The RNG clock mux is never generated.** `RCC.RNGCLockSelection` in the
  `.ioc` produces no `HAL_RCCEx_PeriphCLKConfig` call at all, so the RNG runs
  from its reset default — `hsi48_ck`. What matters is that HSI48 is actually
  enabled; check `RCC_CR` bit 13 on the target rather than trusting the panel.

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
