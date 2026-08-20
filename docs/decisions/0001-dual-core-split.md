# ADR-0001 — CM7 runs the application, CM4 runs the radio

**Status:** Accepted
**Date:** 2026-08-19

## Context

The STM32H755 has a Cortex-M7 at 480 MHz and a Cortex-M4 at 240 MHz. The hub has
two workloads with incompatible timing characters:

- **The radio** will run a TDMA slot grid. A missed deadline is a lost frame, and
  with battery devices a lost frame is a retry that costs more energy than the
  original transmission.
- **The network stack** is throughput-oriented and tolerant of jitter. lwIP holds
  locks, allocates, and occasionally does a lot of work in one call.

## Decision

CM7 runs the application: Ethernet, lwIP, the console, configuration, under
FreeRTOS with CMSIS-RTOS v2.

CM4 runs the radio: RFM69, timing, CRYP. **No RTOS** — a bare superloop with a
free-running microsecond timer.

The split is by **timing class, not workload size**. CM4 uses ~32 KB of flash
against CM7's ~129 KB, and that asymmetry is not a sign of a bad split.

## Consequences

- Radio deadlines cannot be delayed by lwIP, and lwIP cannot be starved by radio
  priority. The question does not arise.
- CM4 stays small and auditable. No scheduler, no dynamic allocation.
- Anything the two must share needs explicit IPC — see
  [ADR-0002](0002-sram4-mailbox.md). This is real ongoing cost.
- **Boot order becomes load-bearing.** CM4 sleeps on `HSEM_ID_0` until CM7 has
  finished clock and peripheral init. `MX_LWIP_Init()` must stay inside
  `StartDefaultTask`; moving it into `main()` deadlocks the boot and presents as a
  dead radio.
- Two images to flash, and a mismatched pair is possible.
- The core with the radio is the core *without* the crypto library — see
  [ADR-0011](0011-mbedtls-on-cm7-only.md). Counterintuitive, and correct.

## Alternatives rejected

**Everything on CM7, CM4 idle.** Simplest, and the radio's deadlines would then be
at the mercy of the same scheduler that serves lwIP. Rejected before the slot grid
made it obviously untenable.

**Radio on CM7, application on CM4.** CM4 is too slow for lwIP at line rate, and it
would put the real-time work on the core with the cache and MPU complexity.

**FreeRTOS on CM4 as well.** A preemptive tick coarser than the deadlines being met
adds uncertainty and buys nothing a superloop cannot do here.

## See also

[architecture/dual-core.md](../architecture/dual-core.md)
