# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Documentation

`docs/` holds the architectural reasoning: why the cores are split the way they are,
why the hop sequence is a shuffle, what the crypto is defending against.
Start at [docs/README.md](docs/README.md); decisions are recorded one per file in
[docs/decisions/](docs/decisions/).

This file stays the short operational brief — build, flash, traps. When a change
alters *why* something is done rather than how, the reasoning belongs in `docs/`
and an accepted decision record is superseded rather than rewritten.

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
set -e                                 # gate the flash on the build; see below
cmake --build --preset Debug
P=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
HUB=sn=0049004A3234510637333934        # the H755 probe; see below
$P -c port=SWD $HUB mode=UR -w CM7/build/testHubFreeRTOS_CM7.elf -v
$P -c port=SWD $HUB mode=UR -w CM4/build/testHubFreeRTOS_CM4.elf -v
$P -c port=SWD $HUB mode=HOTPLUG --rst
```

**Gate the flash on the build's exit code**, not on grepping its output for `error`. A build that
dies some other way greps clean, the flash then succeeds on the previous ELF, and you draw a
conclusion from yesterday's firmware. Prefer checking an invariant that would visibly change - a
printed key, a vector digest - over the absence of errors.

**Always pin the probe.** Bare `-c port=SWD` takes the *first* ST-Link found, and WL55 device
boards share the bus. Confirm with `$P -l`; the hub reads `Device ID 0x450`, `Cortex-M7/M4`, and
has 4 access ports where a WL55 has 2. Same for the console: use
`/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0049004A3234510637333934-if02`, because
`/dev/ttyACM<N>` numbering shifts when other boards are plugged in.

To check the firmware actually runs, read RTOS state over SWD instead of guessing. Get addresses
with `arm-none-eabi-nm`, then `$P -c port=SWD mode=HOTPLUG -r32 <addr> 0x8`. Healthy CM7:
`uxCurrentNumberOfTasks` = 7, `xTickCount` rising, `pxCurrentTCB` non-NULL. On CM4,
`rfm_ms_counter` rising means the TIM7 interrupt is alive.

## Architecture

Dual-core STM32H755 on NUCLEO-H755ZI-Q. The cores split by responsibility and talk over shared
memory guarded by hardware semaphores.

**CM7** — application core. FreeRTOS with CMSIS-RTOS v2, LwIP in RTOS mode (`WITH_RTOS 1`).
Application threads created in `main.c`: `defaultTask` (brings LwIP up, then releases CM4),
`cliTask` (command shell) and `pairTask` (the key exchange — its own 12 KB stack, because a
P-256 scalar multiplication does not fit `defaultTask`'s 2 KB).
**All CM7 traffic to CM4 goes through `hub_ipc_call()`**, which holds a mutex across the whole
transaction. Not just the send: `ipc_poll_reply` drains the ring and discards what does not match
its sequence number, so two pollers eat each other's answers and both time out. LwIP adds `tcpip_thread`, `ethernetif_input` and
`ethernet_link_thread`. CRYP belongs to CM4, not here — the radio is its only consumer.

**Console.** The CLI is on USART3 (PD8/PD9), which is the ST-Link VCP — `/dev/ttyACM0` at
115200 8N1, so it can be driven from a script. `BSP_COM_Init(COM1, ...)` in `main()` configures
the port; `cli_serial_start()` only adds the RX interrupt, which feeds a queue `cliTask` blocks
on. Do not call `getchar`/`scanf`: `_read` in `newlib_stubs.c` polls the same USART and would
race the ISR. UART4 (PC10/PC11) is still configured but no longer used.

**CM4** — radio core. No RTOS; a superloop calling `RFM_Routine()`. It runs the
slot grid (`Common/inc/radio_slots.h`, a hub/device contract in nominal
microseconds) and a three-state pairing machine: `IDLE`, `LISTEN` while an
operator's window is open, and `QUIESCE` for four superframes once a device
actually answers. A quiesce suspends transmission, **not** the superframe
counter — freezing that would repeat GCM nonces, and because the hop sequence is
indexed by the counter, a resumed grid needs no resynchronisation. `device pair`
shows the state and the on-air counters; `device quiesce <n>` forces one for
testing. See `docs/radio/pairing.md`. Drives an RFM69 over SPI1
through the `CM4/rfm69_lib` submodule. `timebase.c` runs TIM2 free at 1 MHz over its full 32
bits as the radio clock — the TDMA slot grid needs microseconds, not the millisecond tick TIM7
used to give. It wraps every ~71 min and every comparison is written to survive that.
**A tick is not a microsecond**: HSE is the ST-Link MCO (X3 unfitted) and was measured between
+3878 and +4600 ppm across one afternoon, so `calib.c` measures
TIM2 against the LSE crystal through TIM16 input capture and the grid steps
`timebase_us_to_ticks()`. Read it with the `timing` command; `docs/radio/timebase.md` has the
reasoning, including what about the measurement is still not understood. CRYP (AES-128-GCM) and RNG are driven from here; both sit in D2 and are reachable from
either core, so RNG is guarded by `HSEM_RNG`.

**Boot order matters.** CM4 enters STOP at reset and waits on `HSEM_ID_0`. CM7 holds that semaphore
across clock and peripheral init, then releases it from `StartDefaultTask` once LwIP is up. Moving
`MX_LWIP_Init()` back into `main()` breaks this — it must run after the scheduler starts.

**Cross-core IPC.** `Common/inc/hsem_table.h` assigns the semaphores; `Common/inc/ipc.h` defines
the rings and `shared_memory.h` the `SHARED_MEM` attribute. Both cores define `shared_ipc` in
`.shared_mem`, which both linker scripts place at the `RAM_D3` origin (SRAM4, `0x38000000`), so the
address is the linker's business rather than a literal. `(NOLOAD)` keeps either core's startup from
wiping what the other wrote. Two SPSC rings, eight slots each, with a magic/version header; SRAM4 is
strongly-ordered under the MPU, so `__DMB()` suffices and no lock guards the data. HSEM is a
doorbell only — CM4 drains by polling. Every request carries a sequence number its reply echoes, so
a late reply cannot be mistaken for the next request's answer. Inspect with the `ipc` command.

**Memory.** RAM_D2 reads as 95% used, but most of it is a hole: `.lwip_sec` starts at the region
origin and the script jumps the location counter to `0x30040000`, so ~256 KB of the section is
linker fill. Real content is the ETH descriptors and the ~19 KB RX pool. The LwIP heap sits at
`LWIP_RAM_HEAP_POINTER 0x30020000` inside that hole — a raw address the linker knows nothing
about, so nothing stops a future section from overlapping it. `0x30000000`..`0x30020000` is free.

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
(`.lwip_sec`) live in the custom files; edit those.

**CMSIS-RTOS v2 takes `stack_size` in bytes, not words.** CubeMX knows this and emits
`512 * 4` in `main.c`, but the LwIP glue does not: `ethernetif.c` and `lwip.c` pass
`INTERFACE_THREAD_STACK_SIZE` straight through, and `sys_arch.c` passes
`TCPIP_THREAD_STACKSIZE` / `DEFAULT_THREAD_STACKSIZE` straight through. Taken as bytes these
are ~4x too small; the first received frame then overflows the `EthIf` stack and hard-faults the
core, which looks like a dead CLI with a frozen `xTickCount`. Check headroom with the `status`
command after pushing traffic — the numbers are words of stack left.

**Known template gaps** to re-apply if regeneration drops them:

- `cmsis_os.h` under CMSIS-RTOS v2 does not pull in the raw FreeRTOS API. Files using
  `xQueueSend`, `pdMS_TO_TICKS`, `pvPortMalloc` etc. need explicit `FreeRTOS.h` / `task.h` /
  `queue.h`.
- `ethernetif.c` still configures a global `TxConfig` in `low_level_init` that the current template
  no longer declares; the declaration is kept in `USER CODE 3`.
- `custom_m7_flash.ld` must match the section names `ethernetif.c` actually emits:
  `.RxDescripSection` / `.TxDescripSection`. The ST community article spells them without the
  `s`, and with that spelling the ETH descriptors silently land in cacheable RAM_D1 instead of
  the non-cacheable window the MPU opens at `0x30040000`.
- **CubeMX will not enable LSE.** Its clock solver prunes any oscillator nothing in its *modelled*
  tree consumes, and a timer's `TISEL` input is not part of that model, so the `.ioc` pin mode
  generates nothing. `LSE_Config()` in CM7's `USER CODE 0` enables it. Same class as the RNG mux.
- **A timer needs its clock-source virtual pin, not just its mode.** TIM16 was silently deleted
  from the `.ioc` until `VP_TIM16_VS_ClockSourceINT` was added beside
  `VP_TIM16_VS_NoInput1`. Nothing appears in the log. Also, the LSE pin mode belongs on **both**
  `OSC32` pins — its `SignalLogicalOp` is `OSC32_IN AND OSC32_OUT`.
- `lwipopts.h` is generated too. `LWIP_STATS`, the thread stack sizes and `MEM_SIZE` are set in
  the CubeMX LwIP panel; re-check them after regeneration.

**Regeneration is scripted.** `.claude/skills/cubemx/SKILL.md` has the headless recipe and the
rule the project follows: anything CubeMX can generate must be generated, not hand-written.

## Addressing

The board boots as a DHCP client (`LWIP.LWIP_DHCP=1`), so `MX_LWIP_Init` brings the netif up on
`0.0.0.0` and the address appears once the PHY has negotiated — allow ~20 s before calling it
broken. `ip dhcp` and `ip static <ip> <mask> <gw>` switch at runtime; the choice is not persisted,
so a reset always returns to DHCP. Both call into lwIP under `LOCK_TCPIP_CORE()` because the CLI
runs outside `tcpip_thread`.

## Testing Ethernet from the host

Serve DHCP on the wired port and keep the default route where it is. NetworkManager's shared mode
runs a dnsmasq for the subnet:

```bash
nmcli con mod "Wired connection 1" ipv4.method shared \
      ipv4.addresses 192.168.137.1/24 ipv4.never-default yes
nmcli con up "Wired connection 1"
ip neigh show dev enp6s0        # the board's lease shows up here
ping -c 200 -i 0.002 <lease>
```

Shared mode also turns on IPv4 forwarding and NAT, which is what gives the board a route out.
For a fixed address instead, use `ipv4.method manual` and `ip static` on the board.

Then read `status` and `lwip` over the console. Healthy: no `drop`/`chkerr`/`err`, `icmp.recv`
matching `icmp.xmit`, and every task with more than ~100 words of stack free.

## Devices and the CLI

`device` replaced `rfm`. `device add <id> <fingerprint>` enrols a device: it persists
the operator's out-of-band fingerprint, assigns the lowest free uplink slot, and only
then opens the pairing window. `device list` shows what is enrolled, `device remove`
tombstones it.

`devices` lists what is actually **paired**, with both RSSI directions — `up`
measured by the hub on the device's last frame, `down` reported by the device
about the hub's last beacon. Both, because a link good one way and not the other
is the common case: the hub transmits at 13 dBm into a real antenna and a sensor
node does not. It also prints the four-frame exchange's counters and CM7's
refusal reasons, so a failed pairing names why instead of "it did not work".
`devices rate <n>` changes what the *next* pairing is granted; there is no
downlink queue, so devices already in the field keep theirs.

**The fingerprint is SHA-256 of the 33-byte compressed SEC1 point.** Not
`0x04||X||Y`, not bare `X`. A 32-byte hash of the wrong domain enrols cleanly and
then fails authentication forever, with the operator having typed exactly what
the device printed — indistinguishable from the attack the check detects.

`CM7/Core/Src/keystore.c` holds those records in bank 1 sectors 6 and 7, same
log-structured shape as the counter store below.

**Never erase bank 1 from CM7.** A 128 KB sector erase there never returns - CM7
fetches from bank 1 - and the interrupted erase leaves the sector with uncorrectable
ECC. `scan()` reads it at every boot before the scheduler, so the board then
bus-faults on *every* power-up with a perfectly good image, and reflashing does not
help. Recovery needs an external programmer:

```bash
$P -c port=SWD $HUB mode=UR -e 6 7      # sectors 6,7 of bank 1
```

This cost the board twice. CM4's equivalent in bank 2 does work, so it is a property
of erasing the bank you execute from. Consequently the CM7 store never erases: the
spare is checked not cleaned, stale-format records are stepped over, there is no
`erase_sector()` in the file, and a full store needs an external erase rather than a
reboot. `ks_init()` still runs before `osKernelStart()` because `scan()` walks 256 KB.

- **The store reports `key_gen` and never judges staleness.** Only the holder of the
  current session key knows which generation is in use; a flag computed in the store
  would always read false, and a flag that is always false looks like a check.
- **Enrolment persists before the window opens.** A window opened for a device whose
  fingerprint did not reach flash is unauthenticated pairing that looks authenticated.
- **The fingerprint is checked now**, in constant time, before any curve work.
  Enrolment is a precondition for authentication and is now spent on it.
- **The append point is the first erased slot**, never one past the last valid record.
  Those differ whenever a slot holds something the scanner rejects, and deriving it
  from valid records alone aims the next write at occupied flash - which an H7 flash
  word refuses, so every write fails from then on with the version gate working
  perfectly.
- **Key rotation is indexed by the superframe counter**, `superframe /
  SUPERFRAME_PER_DAY`, not by wall time - there is no RTC and a clock resetting on a
  power cut would re-derive used keys. `rotate_epoch` in the record is the epoch the
  root key was agreed at, so the current key is derivable and rotation costs no
  flash writes.

## Flash store

`CM4/Core/Src/kvstore.c` keeps the superframe counter across reset, in sectors 6 and 7
of bank 2. It stores a **ceiling** 4096 superframes ahead, not the counter — writing every
2 s would wear the flash out. Nothing at or below a stored ceiling is reused; an unclean
shutdown skips up to 4096 values, which is free against a 2^32 space.

- **Never erase a sector in service.** H7's minimum erase is 128 KB, stalls the bank the
  core executes from, and can take 1.4 s against a 512 ms IWDG2 whose LSI is +/-50%. The
  spare is erased at boot and the log switches to an already-erased sector.
- **The append point is the first erased slot**, in both stores. One past the last
  *valid* record aims the next write at occupied flash whenever a slot holds
  something the scanner rejects. On CM4 that fails the write, latches `exhausted`,
  and silences the radio **permanently** - a reboot erases the spare, not the
  sector holding the bad record.
- **`kv_reserve()` runs only in the first half of the superframe.** A flash program stalls
  the core and the beacon's offset is what every device's period estimate rests on. Measured
  at 2048x the production write rate it costs 3 us of worst case; check with `timing`.
- `device store` reports the ceiling, the margin, and whether writing has stopped.

## The physical layer

`Common/inc/radio_phy.h` is the hub/device PHY contract: the channel grid,
modulation, framing, the CRC variant, and the receive windows each side must
honour. **Every value in it was read back off a chip**, with the register named
beside it, because a driver call that returns success is not evidence a field
holds what was asked for.

Those constants used to live in `CM4/Core/Src/radio.c`, where the device could
not see them and wrote its own copy of each - the arrangement that let
`PAIR_FRAME_LEN` be 45 on one side and 49 here with an assert passing on both.

The file separates **configured** from **derived** from **measured** from
**unmeasured**, and the last section is the point: the receiver's required
preamble and the slot byte's position have never been tested, and are named so
nobody mistakes them for the rest.

## Slots and pairing

The superframe is `beacon | downlink | 96 uplink slots | join region | guard`,
laid out in `Common/inc/radio_slots.h` and checked by `make -C Common/test check`
— which asserts the duty-cycle budget in **both** directions, that the
sustainable combination fits and that the one the code avoids does not.

Traps:

- **The hub's air budget is 20 ms per superframe.** Beacon + downlink + join
  beacon every superframe is 28.5 ms, 1.42%, over the ETSI limit. The downlink
  and the join beacon therefore run at half rate. There is no governor; the hub
  trusts the schedule, so any timing change must be re-measured with the SDR.
- **Never write a slot offset in timer ticks.** A tick is a property of one
  board; the hub converts through `timebase_us_to_ticks()` and a device slaves to
  the beacon.
- **Lead time is not guard band.** The device's oscillator warm-up is known in
  advance and scheduled around by waking early; drift is uncertainty and needs
  guard on both sides. Keeping them apart is why the guard did not move when the
  device's warm-up fell from 10.4 ms to 2.4 ms. Device timings are measured on
  the WL55 bench and have changed once already — cite them, do not bake them in.
- **The quiesce resume superframe is a promise.** It is fixed when announced and
  never extended — 64 devices may be asleep against it. An exchange that
  overruns loses its window, not the schedule.
- **Check the FSM with `device pair`, not with the SDR.** `data_beacons +
  silent_frames` must equal the superframes elapsed. That check does not care
  whether the spectrum is clean, and the spectrum here usually is not.

## Hop sequence

`Common/src/hop.c` picks the channel. It is a **keyed shuffle indexed by the
superframe counter**, not a counter walked upwards and not an LFSR stepped once
per frame:

- The RNG belongs at pairing, where its output becomes the secret both ends
  share. A value drawn per hop would be one only the hub knows, and the device
  could not follow it.
- Indexing by the counter keeps the generator stateless. A node that slept
  through a thousand superframes reads the counter from the beacon and computes
  the current channel directly - nothing to fast-forward, nothing to resync.
- A Fisher-Yates permutation per cycle keeps every channel used exactly once per
  cycle. Taking a PRF or LFSR modulo 29 does not: measured over 1160 frames it
  gave occupancy between 26 and 54 instead of a flat 40, and 3% of hops stayed
  on the same channel.

The PRF is one AES-128 block, run once per cycle - about a minute - through CRYP
on this core. `make -C Common/test check` covers the permutation property, the
stateless jump and the spread.

## Joining

A device that is not paired yet has no key, so it cannot follow the hop
sequence. It finds the hub on a **fixed join channel**: grid slot 14, 866.5 MHz,
reserved out of the 29-slot plan so the hop set is the other 28 and the two
never collide.

The hub only transmits there while an operator has a pairing window open
(`device add <id> <fingerprint>` opens it for 60 s), every second superframe. That costs 0.21% of
duty cycle during the window and nothing at all the rest of the time. A device
that was merely power-cycled does not need the join channel: it still has the
key, and any data beacon gives it the superframe counter.

`radio_join_beacon_t` in `Common/inc/radio_protocol.h` is cleartext by
necessity - it is the one frame a device can read before it has a key - so it
carries only the network and hub id, the superframe counter and the size of the
hop set.

Verified on air: with the window closed there is nothing on 866.5 MHz; with it
open, bursts land there every 4008 ms, which is two superframes.

## Testing the radio from the host

An RTL-SDR on the host validates everything the hub transmits — `tools/sdr/` holds the capture,
demodulator, frame decoder and duty-cycle checker; see its README. It is receive-only, so the
hub's RX path, pairing and ACKs cannot be tested this way.

**Before believing a negative, run a control**: push a frame you know decodes
through the same path. Six false negatives on this bench so far, every one in the
tools rather than the firmware. The recent ones, all now fixed in `tools/sdr`:
a rotate-then-lowpass that deleted the signal; a default offset putting an FSK
tone on the Nyquist edge; `-s 500e3`, a rate the RTL2832U cannot produce, which
`rtl_sdr` warns about and then ignores while `.meta` claims otherwise; and a
burst threshold taken from the peak alone, which lands under the noise because
**the hub reaches this dongle only ~12 dB above it**. `pluck.py` cuts one channel
out of a wideband capture, which is what a hopping transmitter needs.

**Coordinate the air with the device session.** 866.5 MHz is the join channel and
865.1–867.9 the hopping grid; bench traffic belongs on 869.5 with a different
sync word. Say so before measuring duty cycle and let the other side hold
transmit — a bench beacon on the join channel has already cost this project two
debugging sessions.

```bash
python3 -m venv .venv && .venv/bin/pip install numpy
cd tools/sdr
../../.venv/bin/python capture.py cap.iq -f 868e6 -t 5
../../.venv/bin/python decode.py cap.iq
../../.venv/bin/python dutycycle.py cap.iq     # exits 1 if over the ETSI sub-band limit
```

## The radio

**`.claude/skills/rfm69/SKILL.md`** holds everything this project has learned
about the part: the module variants and which PA is bonded, the receive
front-end registers whose reset values are wrong, the SPI clock limit that
corrupts FIFO bursts after CRC has passed, and how to tell a dead receiver from
an empty band. Four hours of "the hub is deaf" was four registers nothing had
ever written, and every one of them returned success.

**Keep that file current.** It is where a new surprise about the radio goes, not
here - this file is the operational brief and the radio has earned its own.

## Known defects

Confirmed by type and control flow, not yet fixed:

- ~~The downlink has never been received.~~ **Opened end to end on 2026-08-20**:
  three frames, three hop channels, every tag verified by the device. Two
  defects sat between "transmitted" and "received", one on each side, and both
  produced counters that were individually correct: the hub sent 93 frames on
  the join channel because it reused `pair_tx()`, and the device opened its
  receiver *at* the region offset while the part was still ramping. `sent 93 of
  93` and `region empty (25000 us open)` were both true statements.
- ~~The pairing exchange has never run against a real device.~~ **Ran end to end
  on 2026-08-20 against a WL55 node**: `req 1 -> rsp 1 -> conf 1 -> accept 1`,
  every refusal counter zero on both sides, and the 19-byte grant - the body
  length that exists to trip the GCM partial-final-word bug - opened correctly
  on an implementation sharing no code with this one. **One device, once.**
- **The data beacon is unauthenticated, so the quiesce flag is a denial-of-
  service primitive.** Bounded by a clamp, a rate limit and the fact that a
  forger must hold the hop key to be on the right channel — none of which is a
  cryptographic guarantee. The fix is a network broadcast key.
- **The hop key is a placeholder until a device pairs.** It is a *network* key —
  16 bytes the hub generates once and delivers sealed in `PAIR_ACCEPT` — and it
  lives in CM7's flash, so for ~3 s after a hub reset CM4 hops under the
  placeholder while CM7 replays its store. A paired device misses a beacon or
  two across a reboot.

  The placeholder is **derived from `hub_id`, not zeros**, and that is the whole
  point of it: the all-zero key is not random, so two unpaired hubs on one bench
  would follow the *identical* sequence and interfere deterministically every
  superframe — the one collision nobody would think to diagnose. Raised by the
  device side. Beaconing on a per-hub sequence beats going silent, because the
  SDR bench this project is verified with has nothing to capture from a silent
  hub.
- **The LSE measurement is unexplained at the window level.** The mean is sound — it matched a
  host-clock measurement to 51 ppm over ten minutes — but a single 7.8 ms window carries ~350 ppm
  of noise that a sixteenfold longer window barely reduced. That should be impossible: the
  accumulated span telescopes to two timestamps. Averaged over ~32 s it does not block the grid.
  `RCC_BDCR.LSEDRV` is at its lowest reset default and is the untested lever.
- **Every timing figure assumes HSE is the ST-Link MCO.** X3 is unfitted. If an HSE crystal is
  ever soldered on, re-measure — the method is in `docs/radio/timebase.md` and needs no
  instruments, just the `timing` command and a host clock.
- **No in-firmware duty-cycle governor.** The hub trusts its schedule rather than counting its own
  air time, so any slot-timing change must be re-measured with the SDR.
- **Before writing CM4's per-frame GCM:** `HAL_CRYP_Decrypt` in GCM mode does not mask the unused
  bytes of a partial final word, while encrypt does. Stale bytes past the payload break the tag on
  decrypt only, so every length not a multiple of four fails with byte-perfect ciphertext — which
  on air looks like a radio fault. Zero the input buffer before the copy. Found on the WL55; the
  23-byte case in `Common/test/vectors/wire_v3` exists to catch it.
**`hop_prf_aes` and the frame cipher share CRYP, and it has already bitten twice
in one afternoon.** CM4 has no RTOS so they cannot interleave — that part is safe
by construction. What is not safe is *inheriting*: `HAL_CRYP_GetConfig` hands
back whatever the other user left.

- The PRF inherited the **key**, which the frame cipher now sets per device, so
  the hop sequence would have followed whichever device last transmitted.
- Then it inherited **`DataWidthUnit`**. The frame cipher sets `BYTE`; the PRF
  passed a length of `4` meaning four *words*, which under `BYTE` encrypts four
  bytes and leaves twelve zeroes. **Fisher-Yates over that produces a perfectly
  valid permutation that no device can follow** — uniform occupancy, correct
  spread, nothing anomalous, and a network that hears nothing.

The second one was in the function whose own comment says inheriting is how two
correct functions produce one wrong answer. **Fixing one instance of a hazard is
not fixing the hazard.** Every user of CRYP must state the whole configuration it
needs: algorithm, key, key size, data width, header size.

**A fault with a duty cycle of 100% looks like a property, not an event.**
`PAIR_INIT` fires every 4th superframe and the join beacon every 2nd, so every
invitation shared a superframe with a beacon - not sometimes, always - and both
were keyed at `join_offset_tk` about 8 ms apart. The device heard 15 beacons and
zero invitations in one 59 s window through one receiver, and read the perfect
regularity as evidence of a systematic fault, which it was: radiated correctly,
on the right carrier, into the shadow of the frame in front of it. Everything
intermittent announces itself by working sometimes; this could not.

Nothing on the transmitting side could see it - `RegFrf` reads back 866.5 MHz
*after* the transmit, `PacketSent` is observed, `tx_err` is zero, `sent` counts
up. **A register read back off the part is evidence about the antenna; a
function's return value is evidence about the function.** Reading `RegFrf` after
the frame went out is what eliminated the carrier hypothesis in one measurement
instead of an hour of argument.

**A decision record can be correct, agreed, and unimplemented**, with nothing in
either firmware disagreeing with it. ADR-0021 said the invitation replaces the
join beacon; both sides agreed it; the replacement was never written. The
document is not a check. It stayed invisible because the ADR carried the
*duty-cycle* argument for replacing and not the reason it is load-bearing, so it
read as an efficiency note rather than a correctness one.

**The mirror of inheriting is asserting.** `pair_tx()` tuned the join channel
itself - deliberately, and its comment says why: it removed a dependency on how
the exchange got there. Reused for the downlink it kept doing that, and 93 frames
went out on 866.5 MHz while the device listened on the hop channel. `sent`,
`tx_err` and `opportunities` all read correct, because the frames really did
transmit. **A send counter cannot see a wrong carrier**, and no readout on this
side could - it took the device naming the grid slots it had listened on. A
function whose name is its caller (`pair_tx`) rather than its contract will be
read as the general verb; make the varying thing an argument.

Only a comparison against an outside AES can catch the second. No check on the
*sequence* can, because the sequence is genuinely uniform. It now runs at boot
(`hop_prf_selftest`, reported as check 20) rather than only when a human types
`hopprf`, and deliberately **after** the frame cipher, so the PRF has to be
correct starting from the state GCM leaves rather than from a fresh init.

**`DataWidthUnit` inheritance is only detectable in one direction.** A *word*
count under a byte configuration truncates — the answer changes and a vector
catches it. A *byte* count under a word configuration over-reads 48 bytes past
the input and still produces the correct block, so no output check can see it.
Both firmwares were bitten in the catchable direction; the other one is a
regression these tests would miss, and the defence is that the live code sets
the field rather than that a test would notice.

- **CRYP's key and IV registers hold big-endian words**, and `DataType` swaps the
  data path only — it does not apply to them. A plain `memcpy` from a byte array
  loads every word reversed, which is a perfectly good cipher under a key nobody
  else has: seal, open, tag verify and tamper-reject all pass. **Only comparing
  against a published frame catches it.** A round trip decrypts its own output
  happily. `aead_selftest()` runs the two `pair_v2` frames through the real path
  at boot for exactly this reason.

**After verifying a check, confirm it reached HEAD.**

```bash
git grep -n _Static_assert HEAD -- Common/inc CM4 CM7
```

The wire-size asserts in `radio_protocol.h` were written, verified non-vacuous by
breaking them, and then destroyed by the cleanup that undid that test -
`git checkout <file>` reverts to HEAD and takes uncommitted work with it. The
commit that followed claimed they were there.

Avoiding that one command is not the fix, because a stash, a failed rebase or an
editor revert all end in the same place. The failure is invisible in the working
tree, invisible in the build and invisible in the firmware: **only HEAD can
disagree with the commit message.** So check the post-condition, not the
procedure. (Copying to the scratchpad and back is still the safer cleanup, and is
why `kvstore.c` and `keystore.c` kept theirs.)

**Know which artifact each assert pins.** "There is an assert" is not the answer:
- `radio_protocol.h` - struct *and* literal both in the shared header, so both
  firmwares compile the same number. This is the one that matters for the wire.
- `ipc.h` - payload sizes against `IPC_PAYLOAD_MAX`, a cross-core contract both
  cores compile from one header.
- `kvstore.c`, `keystore.c` - flash record sizes, correctly local: nobody else
  parses those bytes.

An assert tying two definitions *the same side owns* pins nothing about a
contract. The device side had `PAIR_FRAME_LEN == 45` while this side had 49,
both asserts passing, both internally consistent, and pairing that would have
been refused on length with no diagnosable cause.

**Assert the cost, not only the result.** `timing` counts beacons that leave later
than `RADIO_BEACON_LATE_LIMIT_US` and says so. The append-point bug on the device
side was a page erase on every write that *returned the correct answer* - no
behavioural check could see it, only a cost instrument could. Verified non-vacuous
by rebuilding CM4 with the limit below the normal 4-9 us and watching it fire.

**Three neighbours that are easy to confuse.** Worked out jointly with the device
session across a week of finding all three:

- a **vacuous** check does nothing and looks like it does something — `sizeof`
  against itself, or a length each side defines for itself;
- a **decorative** refusal is computed correctly and nothing acts on it — the
  store's exhausted-log path, which returned an error while the counter marched
  past the ceiling;
- a check whose **name is broader than its coverage** does something real, and
  the danger is that it reads like the general property so the next person stops
  looking. `hub_eph != hub_static` rejects one specific way of being unfresh and
  will be remembered as "the device checks the ephemeral is fresh".

- a check whose **own setup destroys the condition it tests** — the give-away is
  ordering, where the first statement undoes what the second was meant to
  observe.

The third is hardest to *find*: it passes review, passes tests, and is genuinely
doing work. The fix is never to delete it, only to name it honestly.

**The success-path pattern, which is the failure-path one inverted.** Five
defects found the evening pairing first worked lived in code that could only
execute once something finally went *right*: `GET_DEVICE_INFO` read its index
from the wrong byte and had failed every call since it was written, because
printing a row needs a paired device; the device side's grant was never
persisted; its `hop` command answered with the test-vector key. Nothing had
ever reached them. Grep for the paths that only a success unlocks whenever a
first success is imminent - that is the cheapest moment they will ever be found.

**A number is a fraction of a population, and both halves must come from the
same one.** Four times in one evening a numerator and a denominator that were
each individually correct were combined across different windows, different
runs, or different builds: nine syncs against a transmit log that had stopped
being written, a loopback's *passes* compared against a frame's *bytes*, three
hop channels quoted with a key read from a different boot. **Each half survives
any check applied to it separately**, which is why nothing catches it but asking
what the denominator is a fraction of. The worst of the four produced not a
wrong answer but a wrong *dismissal* - a working instrument argued out of the
evidence pile, and a bad conclusion at least gets tested.

**A counter can rise during the very event whose absence you are using it to
detect.** The device read "two beacons heard" as proof no quiesce was armed -
and the quiesce *announcement* is a beacon. Locally valid at every step, and the
instrument behaving exactly as designed.

**An instrument that has never once passed cannot be read in either direction.**
`sync` read 0 for its whole existence; `device spiloop` failed 200 of 200 on
first run. A counter that has never been non-zero is indistinguishable from one
that cannot be, and a test that has never gone green is indistinguishable from
an invalid test - which is exactly how the loopback got argued away half an hour
before it proved the fix. **Ship the control with the instrument**, not after it
disappoints: `spiloop`'s register arm is what made its FIFO arm readable.

**Say what an instrument has not yet been shown to do, before reading anything
out of it** - and name the specific untested half, not the general uncertainty.
"This is new code, it might be wrong" buys nothing. "This has never keyed up, so
a silent hub is my bug before it is yours" tells the other side which branch to
attribute to whom, and one window discharges it.

A sixth lives outside the code entirely: a **report of a verification can be
broader than the verification**. The device side ran `git checkout -- <file>`,
`git status` and a grep, then wrote "byte-identical to `5f7d784`" — naming a
commit it had not compared against, which predated the file. The check was real;
the sentence was not, and the receiving end cannot tell the two apart. The
defence is to say what was actually run, because that sentence cannot be broader
than the check — it *is* the check.

The fourth is hardest to *believe*, because it is none of the other three. It
runs real code on real inputs, something acts on its result, and its name
matches its intent exactly. The device side wrote a check that CRYP survives a
hostile configuration as `gcm_open(...) == 0 && ecb_block(...) == 0` — and the
GCM open re-initialises the whole config, so the ECB never saw the hostile state
at all. **Nothing but mutation finds this.** Reading it, it is correct; running
it, it passes. It is found by deleting the defect's cause and watching the check
stay green. Note the fix differs from the other three: a vacuous check needs
replacing, this one needs **reordering**.

**Round-tripping tests the round trip, not the assembly.** Seal-then-open agrees
with itself under any self-consistent nonce, AAD or key packing. Only matching a
*published* frame proves the assembly — which is the one place the wire's
little-endian struct fields meet the crypto's big-endian inputs. Both endiannesses
are live inside a single frame: `hub_id` is `33 44 22 11` in the GCM salt and
`11 22 44 33` in the header of the frame carrying it. That is not fixable without
breaking a beacon decode verified on air, so whole frames are pinned in
`Common/test/vectors/pair_v2.txt` and nobody has to infer which rule applies.

**When a test length is chosen to catch a known defect, check that a length
outside the defect's class still passes.** The 19-byte grant exists to trip the
GCM partial-final-word bug; the 8-byte uplink report exists to stay green while
it trips. A test that failed on both would be reporting something else.

**A contract that changes width needs the width in its signature.** `uint8_t *fp`
is not a width and the compiler cannot see a too-small caller. `uint8_t fp[32]`
*is* — GCC diagnoses it through `-Wstringop-overread`, with no `-Wall` needed, as
long as the caller's array size is visible. Keep a length parameter as well for
callers that genuinely pass a pointer: two bounds catching two different callers,
not belt and braces.

**A vector set with one instance of a field tests its format and never its
source.** `pair_v2.txt` carries a single `pair_req_superframe`, so inside it the
request's superframe, the live counter and the last beacon's are the same
number - and an implementation reading any of the three reproduces the vector
forever. Both firmwares passed it and could still disagree on air, which is what
pair_v3 exposed: the invitation and the beacon no longer share a superframe, and
the coincidence that had been doing the work was never named. Same family as the
consumer-less vector below - the value reproduces and disagreement is
unreachable. A field whose provenance matters needs **three distinct values** in
the vectors, one per plausible source, so the wrong read fails a host test
instead of an air test. `Common/test/vectors/pair_prov` is that set: pair_v2's
exchange with only the superframe moved - the generator checks the transcript
differs from pair_v2's at offset 8..11 **and nowhere else** - carrying the
request's value, a plausible last beacon's and a plausible live counter, all
three legitimate at the same instant. It publishes what each *wrong* read
produces, so a failure names the source instead of reporting a mismatch. Its
consumer is `superframe provenance` in `crypto`, which derives twice: once for
the answer and once to prove the decoy is really what the wrong source gives.
On this side that pins the binding and not the provenance - the derivation
takes the superframe as a parameter, so the wiring from frame to call spans two
cores and no self-test reaches it. Said out loud in the file and the test,
because a vector believed to cover more than it does is worse than one covering
less.

**And the set covers provenance on neither side, because the device side fixed
it structurally instead.** It had the superframe as a parameter to two
consumers and updated one of them; the fix was not to test for the wrong value
but to delete it - both consumers now read the same struct field, so **there is
no second value in scope to pass by mistake**. A test finds a caller passing the
wrong value; removing the parameter means there is no wrong value to pass.
Prefer that whenever the choice exists, and re-read what a test claims after
someone removes its subject: this entry asserted a coverage that was true when
written and false an hour later, with the vector unchanged. What survives is
real and smaller - the format and the binding on both sides, plus **diagnosis**:
publishing what each wrong read produces, so a future disagreement names its
source instead of reporting a mismatch. That half generalises to any value with
more than one plausible source.

**A vector whose consumer does not exist is untested in the way that matters.**
`key_hop_gen0` reproduced byte for byte for weeks and proved only that HKDF works
— nothing consumed it, so nothing could disagree. That is how the pairwise-hop-key
defect survived cross-verification by two independent implementations. The
converse holds too: a vector whose consumer exists but whose *primitive* is never
compared against an outside answer is equally blind, which is what let the hop PRF
encrypt four bytes of sixteen.

**A digest over a vector file must cover the values, not the text.** All three
generators hashed their emitted output, so a reworded comment moved the digest of
a set whose values could not have changed and anyone holding the old constant saw
"the vectors changed" about something that had not. The false alarm is in the
worse direction — a stale constant fails a build, a redundant diff wastes a
minute — and it manufactured a fake dilemma about whether prose corrections need
a new version. They do not. `value_digest()` in each generator sorts by key and
drops comments; the rewrite guard still compares the whole text, because the
guard exists to make someone think and the digest exists to tell consumers
whether anything they depend on moved.

**A value that is correct for a *different* computation reads as reassuringly
familiar.** `5e102fa24f59aaf3` was hop_v1's text digest and there is no path back
to it under a value digest, but it looks like the right answer. Same shape as a
verification reported against a commit that predates the file, and as a "FIPS-197
constant" that was actually produced by the same library it is meant to check.

**`git checkout` discipline attaches to the operation, not the file type.** Both
sides used scratchpad copies religiously for C files all afternoon and reached
for `git checkout --` the moment the file was Python or a generator. The device
side lost uncommitted work to it in the exact command this file warns about. The
failure was invisible in the tree, in the run and in the output — the generator
ran and printed a digest — and was found only by grepping for a symbol expected
to be there. Check the post-condition, not the procedure.

**Any probe needs its negative case exercised once.** `make -q check` on a
`.PHONY` target returns 1 unconditionally, so it cannot distinguish its two
outcomes and the verification never happened - nearer the vacuous class than the
reporting one, and living in the harness rather than the code. The forms that
mean something all look the same: `make -q` against a real target reading 0/1/0
across settle-touch-rebuild, a mutation that must fail, a test length outside the
defect's class that must pass. Found by the device side, in its own harness.

**A guard can fail in the direction that guarantees the thing it prevents.**
`ps -eo pid,cmd | grep "[w]inhold"`, meant to stop a second window-holder
starting, **matched its own shell**: the wrapper's argv contains the whole
script, pattern included. It printed "ALREADY RUNNING", started nothing, and the
pairing window expired in silence. The bracket trick defeats grep matching
itself and does nothing about the shell around it. Note which way it failed - a
duplicate-suppressor that instead guarantees absence, and reports success doing
it. Match on the process *name* as well as the pattern
(`$2 ~ /python/ && /winhold\.py/`). The `pkill -f` form of this had already
cost two exits earlier the same session: **fixing one instance of a hazard is
not fixing the hazard**, and the read-only variant reads as harmless right up
until the thing it guards is the only thing holding a window open.

**A digest in a generated artifact detects regeneration, never tampering.** The
value is a literal baked into the file it describes, so a hand-edited header
keeps its old digest and every consumer compiles the tampered values and agrees.
Measured: editing two bytes of `HV_DECK0` leaves `vectors` reading `ok` on both
cores and fails the host deck test on the first assertion. **The values are the
check; the digest is a label.** `vectors` therefore prints what it covers on
every run - it compares what each core was *built* with and validates nothing -
because an operator who reads "ok" twice will otherwise carry away that the
vectors were verified.

**After any finding, check whether it applies here too.** Three times this week
a defect spotted in the other repository was sitting in this one — the
append-point bug, the wire-length assert, the self-test naming a moved contract.
The sweep is cheap and the instinct is not automatic.

**The failure-path pattern.** Five defects so far between this repo and the device
repo lived in code that only runs when something is already wrong: a guard whose
refusal nothing acted on, a discarded return, a half-guard that read as a whole one.
They are found faster by grepping for the *pattern* - discarded returns, `(void)`
calls, guards with no matching enforcement, positions derived from a validity-filtered
scan - than by re-reading the module. Do that sweep when adding anything with an
error path, and **sweep for the class rather than the instance**: the append-point bug
existed in both stores and was fixed in one of them an hour before anyone thought to
check the other.

Fixed this session, listed because the shape recurs: the malformed broadcast frame; Manchester
coding doubling air time; unbounded DIO polling; IWDG2 being off; `RNG_SR` keeping `SEIS` latched;
the single-slot IPC mailbox; the superframe counter advancing only on a successful transmit; the
hop PRF running word-swapped with a little-endian cycle counter.
