# Timebase and its calibration

**Status: implemented and measured on target.**

The radio's clock is TIM2, free-running at a nominal 1 MHz across its full 32
bits — [ADR-0006](../decisions/0006-microsecond-timebase.md) covers why it is
free-running and how every comparison survives the ~71 minute wrap. This page is
about the other half: a tick is not a microsecond, and what the firmware does
about it.

## The reference is wrong by 0.4%

`HSE` comes from the ST-Link MCO, not a crystal — `RCC_HSE_BYPASS` in
`SystemClock_Config`, because the NUCLEO ships with no HSE crystal fitted.

TIM2 is configured correctly: 200 MHz timer clock, prescaler 199, exactly 1 MHz
on paper. Measured against the host clock over 597 s it runs at **1 003 878 Hz**
— **+3878 ppm**.

Nothing in the code is wrong. The number the code is counting is.

This matters because the TDMA guard band is drift, and drift is measured in ppm
of the interval since a device last resynced. [tdma.md](tdma.md) works the
budget: at ~4000 ppm the grid holds 57 devices against a target of 64.

## The only accurate reference on the board

The 32.768 kHz LSE crystal is fitted and oscillating. That was established
before any code was written for it, by writing `PWR_CR1.DBP` and `RCC_BDCR.LSEON`
over SWD and watching `LSERDY` come up — `RCC_BDCR` reading `0x3`.

A crystal is ~20 ppm. The MCO is ~4000. So the LSE is worth two orders of
magnitude, and it costs no hardware change.

## How the measurement is taken

**TIM16 timestamps LSE edges.** Its TI1 input is connected to LSE *inside the
chip* through the `TISEL` register, so the reference needs no pin, no wire and no
board change:

```c
HAL_TIMEx_TISelection(&htim16, TIM_TIM16_TI1_RCC_LSE, TIM_CHANNEL_1);
```

TIM16 runs unprescaled at 200 MHz with `ARR = 65535`, and the input capture
prescaler divides by 8, so one capture arrives every 8 LSE periods — 244.14 µs,
about 48 828 ticks, comfortably inside the counter's 65 536-tick wrap.

TIM16 and TIM2 are fed from the same PLL1 tree. Whatever error TIM16 measures is
the error TIM2 carries, so measuring one calibrates the other.

**The count of LSE periods is never inferred.** It would be easy to work out how
many periods a gap covered by dividing elapsed time — and wrong, because that
reasoning is circular when the clock doing the timing is the thing being
measured. Instead the hardware reports it: `CC1OF`, the overcapture flag, sets
whenever a timestamp is overwritten before it was read. If it is set, the window
is discarded. If it is clear, every span is exactly 8 LSE periods.

**A span is checked as well.** One period out is 12% off nominal, so a ±2% check
catches any miscount the flag somehow missed.

**The counter is left alone.** Correcting TIM2's prescaler was never an option:
it is an integer divider off 200 MHz, so it steps 0.5% at a time against an error
of 0.35%. The schedule is converted instead:

```c
uint32_t timebase_us_to_ticks(uint32_t us);   /* Q24 scale, nominal until measured */
```

The superframe grid steps the result. Timeouts keep using raw ticks, where a
0.4% error is meaningless.

## Averaging, not tracking

A single 7.8 ms window carries roughly **350 ppm** of noise. Making the window
sixteen times longer barely helped, so the noise is not something a longer
measurement fixes.

The mean is another matter. Averaged over ten minutes, the measurement agreed
with the host-clock figure to **51 ppm** — unbiased, just noisy.

So the published scale is an average: an exact running mean over the first 4096
windows, then an exponential average carried on the same accumulator, about a
32 s time constant. Tracking the most recent window instead would put ±0.7 ms of
jitter on a 2 s superframe, which is the same order as the guard band this whole
exercise exists to shrink.

The grid re-reads the period at each superframe boundary, so a refined
measurement moves the next interval and never the one already being timed.

## Conditions these numbers were taken under

**Every figure on this page was measured with `HSE` driven by the ST-Link's
8 MHz MCO** — `RCC_HSE_BYPASS`, because the NUCLEO-H755ZI-Q ships with X3
unfitted. That is the defect being corrected, so it is also the thing that makes
the numbers what they are.

**If a real HSE crystal is ever fitted, all of this has to be re-measured.** With
a crystal the offset drops from ~4000 ppm to ~10-20 ppm and the drift largely
goes with it, which changes three things:

- the ppm figures below become historical, not descriptive;
- the slot-budget comparison in [tdma.md](tdma.md) loses its first row — the
  argument for calibrating *before* laying out the grid was specific to a 4000
  ppm reference;
- the calibration itself stays worth having, but stops being load-bearing. It
  would then be correcting a crystal against a crystal.

What would **not** change is the per-window noise described under *What is not
established*: that sits on the LSE side of the measurement, not the HSE side.

Observed across one afternoon on the bench, undisturbed: **+3878, +4202, +4442,
+4600 ppm**. The MCO wanders over roughly 700 ppm at rest, which is the single
strongest argument for measuring continuously rather than baking in a constant.

## Method

Reproducible from the host with no instruments. The board reports raw TIM2 ticks
precisely so the calibration can be checked against something outside itself.

**1 — the true tick rate.** `timing` prints `ticks`, the raw TIM2 counter. Two
reads a long way apart, divided by host wall-clock time, give the real rate:

```
rate_Hz  = (ticks₂ - ticks₁) / (t₂ - t₁)      # unsigned, TIM2 wraps at 2³²
true_ppm = (rate_Hz / 1e6 - 1) × 1e6
```

Use **at least 10 minutes**. The serial round trip is a few milliseconds, so
600 s bounds this to single-digit ppm. TIM2 wraps every ~71.6 min, so keep the
interval under that or track the wrap.

**2 — what the firmware thinks.** The same replies carry `clock … ppm`, the
filtered scale. Average it across the run. Sample only **after ~45 s**, or the
exact-running-mean phase is still converging and drags the average.

**3 — the residual is the answer.** `published_ppm − true_ppm`. This is the
number that matters, because the real superframe is
`grid steps / rate_Hz`, and the two errors cancel exactly when the residual is
zero. Measured: **−27 ppm**.

Do **not** measure the period by counting the `superframe` field over wall clock.
It only advances every 2 s, so a first/last difference carries ±1 frame of
quantisation — about ±3000 ppm over ten minutes, which swamps what is being
measured. That mistake produced two meaningless readings before it was caught.

The on-air beacon cadence via [SDR](../testing/sdr.md) is a genuine independent
check but a coarse one: burst timing resolves to roughly ±250 ppm, an order of
magnitude worse than the host-clock method above.

## What it achieves

Measured on target, host clock as the reference, 711 s:

| | |
|---|---|
| TIM2 true rate | 1 004 202 Hz, **+4202 ppm** |
| scale the firmware published | **+4175 ppm** |
| residual | **−27 ppm** |
| superframe, real time | **1999.945 ms** |

Against 1992.5 ms before calibration, the error falls from −3750 ppm to −27 ppm.

**The reference drifts, so this cannot be a constant.** The same measurement
taken half an hour earlier gave +3878 ppm, not +4202 — the MCO moved 324 ppm
while the board sat on the bench. A one-shot calibration baked in at boot would
have been wrong by more than ten times the crystal's own accuracy by the end of
the afternoon.

## Reading it

```
> timing
superframe 462, period 2000000 us nominal
beacon late: last 2 us, min 1, max 3 (spread 2)
clock +4442 ppm vs nominal, grid steps 2008853 ticks
calib: 117762 windows, 0 rejected
       spread +2435..+5996 ppm
       last window spans 49031..49079 ticks
ticks 929618898
```

- `clock` is the filtered scale — what the grid actually uses.
- `spread` is the **raw** per-window range, deliberately not filtered. It is the
  quality of the input, and it is reported rather than smoothed away because two
  real defects were caught by watching it move.
- `ticks` is raw TIM2. Two reads far apart give the true tick rate against the
  host clock, which is how the calibration is checked instead of believed:

```bash
# ~10 minutes apart, then (Δticks / Δseconds) is the real TIM2 rate
```

- `calib: N windows, 0 rejected` with N at zero means LSE never came up; the grid
  is then running on the nominal period and says so.

## What is not established

**Where the per-window noise comes from.** It is not endpoint jitter: the
accumulated span telescopes to the difference between the first and last
timestamp, so a sixteenfold longer window should have divided the noise by
sixteen, and it did not. It is not a miscounted span either, since every span is
within ±2% and one LSE period out is 12%. `RCC_BDCR.LSEDRV` sits at its lowest
reset default and is the obvious untested lever.

It does not block the grid — the mean is unbiased, and averaging is what the
design leans on — but it is not understood, and a claim that it is would be
wrong.

## CubeMX will not generate the oscillator

`LSE-External-Oscillator` is set on both `OSC32` pins in the `.ioc`, and CubeMX
still emits no LSE in `SystemClock_Config`. Its clock solver prunes any
oscillator nothing in its *modelled* tree consumes, and a timer's TI selection is
not part of that model. The `.ioc` keeps the pin mode so the GUI shows the truth,
and `LSE_Config()` in CM7's `USER CODE 0` does the enabling.

This is the same class of gap as the RNG clock mux, and both are listed in
`CLAUDE.md`.

Two further traps, since the `.ioc` edit is scripted:

- **TIM16 was silently deleted** on the first attempt. A timer needs its
  clock-source record — `VP_TIM16_VS_ClockSourceINT` — as well as the capture
  mode's `VP_TIM16_VS_NoInput1`. With only the latter the IP is incomplete and
  disappears with nothing in the log.
- **The LSE mode belongs on both pins.** Its `SignalLogicalOp` is `OSC32_IN AND
  OSC32_OUT`, unlike the HSE clock-source mode which needs only `OSC_IN`.

## See also

[tdma.md](tdma.md) — the slot budget this feeds ·
[ADR-0019](../decisions/0019-lse-disciplined-timebase.md) ·
[ADR-0006](../decisions/0006-microsecond-timebase.md) ·
[architecture/build-and-generation.md](../architecture/build-and-generation.md)
