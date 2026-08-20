# ADR-0006 — Free-running 32-bit microsecond timebase on TIM2

**Status:** Accepted
**Date:** 2026-08-20

## Context

The radio previously took its time from TIM7's millisecond tick. A TDMA slot grid
needs finer resolution than that: a millisecond is already a large fraction of the
guard interval between adjacent slots, so a millisecond clock cannot express the
schedule it is meant to enforce.

## Decision

TIM2 runs **free at 1 MHz across its full 32 bits**. It is never reloaded, never
reset, and has no period interrupt.

Every comparison against it is written to survive the wrap:

```c
uint8_t timebase_elapsed(uint32_t deadline_us) {
    return (int32_t)(rfm_micros() - deadline_us) >= 0;
}
```

Unsigned subtraction, then a **signed** comparison.

## Consequences

- Microsecond deadlines are expressible, which the slot grid requires.
- 32 bits at 1 MHz wraps every **~71.6 minutes** and the hub runs for months, so
  wrap handling is not optional. The idiom above is correct for any interval
  shorter than ~35 minutes, which every radio deadline is by a wide margin.
- **The naive form is a trap.** `rfm_micros() >= deadline_us` works for 71 minutes
  and then fails once — the worst possible failure shape, because it survives all
  testing and breaks in the field.
- No interrupt, so reading the time costs a register read and cannot be preempted
  mid-value.
- TIM2 is a 32-bit timer and is therefore spent on this. Acceptable; CM4 has
  little else to do with it.

## Alternatives rejected

**Keep the millisecond tick.** Cannot express slot timing.

**A 16-bit timer with an overflow counter.** Wraps every 65 ms, so the overflow ISR
must never be missed — and it is exactly the thing a busy superloop can miss.
A 32-bit timer removes the failure mode instead of managing it.

**DWT cycle counter.** Free-running and 32-bit, but wraps every ~18 seconds at
240 MHz and is a debug unit that a debugger may reset underneath the firmware.

## See also

[radio/tdma.md](../radio/tdma.md)
