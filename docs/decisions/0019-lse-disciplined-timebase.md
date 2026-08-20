# ADR-0019 — Discipline the timebase against LSE, by averaging rather than tracking

**Status:** Accepted. Measured on target.
**Date:** 2026-08-20

Extends [ADR-0006](0006-microsecond-timebase.md), which chose the free-running
1 MHz TIM2 timebase. That decision stands; this one corrects what a tick is
worth.

## Context

`HSE` on the NUCLEO-H755ZI-Q is the ST-Link MCO (`RCC_HSE_BYPASS`), not a
crystal. TIM2 is configured for exactly 1 MHz — 200 MHz timer clock, prescaler
199 — and measured against the host clock over 597 s it runs at **1 003 878 Hz,
+3878 ppm**.

That is a capacity problem, not a comfort one. A TDMA guard band has to cover
the drift between hub and device since their last resync, and at ~4000 ppm the
grid spends its slots on guard: **57 against a 64-device target**. See
[radio/tdma.md](../radio/tdma.md).

The board's 32.768 kHz LSE crystal is fitted and oscillating — probed over SWD
before any of this was built. It is the only accurate reference present.

## Decision

**Measure, do not re-tune.** TIM2 keeps its prescaler. Its prescaler steps 0.5%
at a time against an error of 0.35%, so it cannot express the correction anyway.

**TIM16 timestamps LSE edges.** Its TI1 is connected to LSE inside the chip
through `TISEL`, so the reference needs no pin and no wire. TIM16 and TIM2 hang
off the same PLL1 tree, so the ratio TIM16 measures is the error TIM2 carries.

**The schedule is converted, not the clock.** `timebase_us_to_ticks()` turns
nominal microseconds into real ticks through a Q24 scale, and the superframe
grid steps that. Timeouts keep using raw ticks, where 0.4% does not matter.

**Average across windows; do not track the last one.** A 7.8 ms window carries
about **350 ppm** of noise, and lengthening the window sixteenfold barely moves
it. The mean, however, is sound: averaged over ten minutes it agreed with the
host-clock measurement to **51 ppm**. So the published scale is an exact running
mean for the first 4096 windows and an exponential average of the same
accumulator after that — about a 32 s time constant.

## Result

Measured over 711 s against the host clock: TIM2 true **+4202 ppm**, scale
published **+4175 ppm**, residual **−27 ppm**, superframe **1999.945 ms**. The
error falls from −3750 ppm to −27.

The same measurement half an hour earlier read +3878 ppm, so the MCO drifted
324 ppm on a bench at rest; across the afternoon it ranged +3878 to +4600.
**That settles the calibration as continuous rather than a boot-time constant**,
which was not obvious before it was measured.

**These figures are conditional on the clock source.** `HSE` is the ST-Link
8 MHz MCO because X3 is unfitted. If a real HSE crystal is ever soldered on, the
measurements must be redone: the offset falls to ~10-20 ppm, most of the drift
goes with it, and the argument that calibration had to precede the slot grid
stops applying. The decision to convert the schedule rather than retune the
counter still holds; its urgency does not. Method for re-measuring is in
[radio/timebase.md](../radio/timebase.md).

## Consequences

- The superframe is 2.000 s of real time to within 27 ppm instead of 1992.5 ms,
  and the guard band is set by the LSE's ~20 ppm instead of the MCO's ~4000.
- **Devices no longer have to compensate for a defect in the hub.** A device
  could in principle cancel a static hub offset by measuring beacon-to-beacon
  intervals and scaling its slot offsets. Requiring that of all 64 devices, to
  work around one wrong clock, is the wrong place to fix it — and it fails
  exactly when a device most needs the grid: on first join, and after a run of
  missed beacons, when there is no recent interval to measure.
- **Boot is slower.** LSE start-up is seconds of crystal ramp before `LSERDY`.
- **A dead crystal degrades rather than hangs.** `calib_init()` takes one window
  with a timeout, then gives up; the grid falls back to the nominal period and
  `timing` reports zero windows.
- The scale moves between superframes, so the grid period is re-read at each
  boundary and never mid-interval.

## The noise is published, not smoothed away

`timing` prints the filtered value *and* the raw per-window spread. A sound
measurement sits inside a few ppm of the last one; the observed spread is
~2000 ppm wide, which is the honest state of it. Hiding that behind the filter
would have hidden the two real defects found while building this:

- a window that kept a **stale capture timestamp** across a gap, so one span
  covered an unknown number of LSE periods — and still passed a ±6% tolerance,
  which is why `rejects` stayed at zero while the result moved by hundreds of
  ppm;
- an in-place Q24 running average whose correction **truncated to zero** once it
  fell below the divisor, so the last 244 ppm could never arrive.

Neither would have been visible in a filtered output.

## Open

The source of the per-window noise is **not established**. It is not endpoint
jitter — the accumulated span telescopes to the difference of two timestamps, so
a sixteenfold longer window should have cut it sixteenfold and did not. It is
not a miscounted span either: every span is checked against ±2%, and a span off
by even one LSE period is 12% out. LSE drive strength (`RCC_BDCR.LSEDRV`, at its
lowest reset default) is the untested lever.

It does not block the grid, because the mean is unbiased and averaging is what
the design relies on.

## Alternatives rejected

**Re-tune the TIM2 prescaler.** Integer, 0.5% steps. Cannot express 0.35%.

**Clock the system from LSE through a PLL.** Gives up HSE's speed for the whole
chip to fix one timer, and 32.768 kHz is a poor PLL reference.

**Track the last window instead of averaging.** With 350 ppm of per-window noise
this puts ±0.7 ms of jitter on a 2 s superframe — the same order as the guard
band the calibration exists to shrink.

**Leave it, and have devices measure the hub's rate.** Rejected above.

## See also

[radio/timebase.md](../radio/timebase.md) · [radio/tdma.md](../radio/tdma.md) ·
[ADR-0006](0006-microsecond-timebase.md)
