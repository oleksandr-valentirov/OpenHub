# On-target inspection

Checking that the firmware runs, by reading the target rather than inferring from
the code.

## Flash

CM7 lives at `0x08000000`, CM4 at `0x08100000`; writing the ELF picks the address
up automatically.

```bash
P=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
HUB=sn=0049004A3234510637333934
$P -c port=SWD $HUB mode=UR -w CM7/build/testHubFreeRTOS_CM7.elf -v
$P -c port=SWD $HUB mode=UR -w CM4/build/testHubFreeRTOS_CM4.elf -v
$P -c port=SWD $HUB mode=HOTPLUG --rst
```

## Pin the probe, always

Sensor-device work happens on WL55 boards on the same USB bus, so several ST-Link
probes are usually connected. **`-c port=SWD` without `sn=` selects the first
probe enumerated, which is not necessarily the hub** — the ordering is not stable
and does not follow plug order.

Identify with `$P -l`, then confirm the target before writing:

| | Hub (H755) | Device (WL55) |
|---|---|---|
| Device ID | `0x450` | `0x497` |
| CPU | Cortex-M7/M4 | Cortex-M4 |
| Access ports | 4 | 2 |

The console has the same problem: `/dev/ttyACM<N>` numbering shifts when other
boards are plugged in. Use the stable path instead:

```
/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0049004A3234510637333934-if02
```

Avoid probe-global operations while the bus is shared — mass erase without `sn=`,
DFU commands, ST-Link firmware updates. `-l` is read-only and safe.

## Reading RTOS state over SWD

Get symbol addresses with `arm-none-eabi-nm`, then read memory:

```bash
$P -c port=SWD mode=HOTPLUG -r32 <addr> 0x8
```

| Symbol | Core | Healthy |
|---|---|---|
| `uxCurrentNumberOfTasks` | CM7 | 8 |
| `xTickCount` | CM7 | rising between reads |
| `pxCurrentTCB` | CM7 | non-NULL |
| `rfm_ms_counter` | CM4 | rising — the timer interrupt is alive |

A frozen `xTickCount` with a live debug connection is the signature of a hard
fault, and the most common cause in this project is the
[lwIP stack-size trap](../network/ethernet.md#the-stack-size-trap).

## Console checks

On `/dev/ttyACM0` at 115200:

- `?` — commands respond at all
- `status` — task table and stack headroom, in **words**. Anything under ~100 words
  free is a problem, especially after pushing traffic.
- `lwip` — statistics; no `drop`, `chkerr` or `err`, and `icmp.recv` matching
  `icmp.xmit`
- `rng` — draws through the guarded RNG service and prints `RNG_SR` before and
  after. A `0x41` on entry is normal (the flag latches while idle); the closing
  line must read `healthy`. See [security/entropy.md](../security/entropy.md).
- `device dump 10` — returns `0x24` if CM4, the SPI bus and the
  [cross-core IPC](../architecture/ipc.md) are all working. This is the single most
  useful one-line health check, because it exercises the whole chain.

## Watchdog note

CM4 feeds IWDG2 from the radio superloop. If the core resets every ~512 ms, the
refresh is missing or the loop is blocked. Confirm by watching the air cadence with
the [SDR bench](sdr.md) — a resetting core produces a visibly broken transmit
rhythm.
