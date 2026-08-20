# TDMA and timing

**Status: timebase, superframe cadence and slot geometry implemented; the slot
scheduler that assigns and serves them is not.**

## Timebase

TIM2 runs **free at 1 MHz over its full 32 bits** as the radio clock. It is never
reloaded and never reset.

The previous design used TIM7's millisecond tick. A slot grid needs microseconds:
a millisecond is already a quarter of the guard budget between adjacent slots, so
a millisecond clock cannot express the schedule it is supposed to enforce.

32 bits at 1 MHz wraps every **~71.6 minutes**, and the hub runs for months.
Every comparison is therefore written to survive the wrap:

```c
uint8_t timebase_elapsed(uint32_t deadline_us) {
    return (int32_t)(rfm_micros() - deadline_us) >= 0;
}
```

Unsigned subtraction then a **signed** comparison. This is correct across the wrap
for any interval shorter than half the period (~35 minutes), which every radio
deadline is by a wide margin. Writing `rfm_micros() >= deadline_us` instead would
work for 71 minutes and then fail once, which is the worst possible failure shape.

Rationale: [ADR-0006](../decisions/0006-microsecond-timebase.md).

## Superframe

**Implemented, on an absolute grid.** The boundary is a fixed step from the
*previous boundary*, never from whenever the work finished:

```c
superframe_start_us += SUPERFRAME_US;
frame_counter++;
```

Adding the period to "now" lets transmit time accumulate as drift, which is a
grid no device can predict a slot on. It also let the counter advance only when a
transmission succeeded — so a radio error stalled the protocol's clock and
repeated a superframe number, which once frames are sealed is **nonce reuse**.
The counter now advances at the boundary and nowhere else.

### The beacon must leave at a fixed offset, and does

Devices measure the superframe period from **consecutive beacons** rather than
being told it ([protocol reasoning](../security/wire-crypto.md)). That quietly
requires the beacon to leave at a consistent offset within the superframe: any
jitter in the transmit moment lands directly in every device's period estimate,
and from there into its free-running drift between beacons.

The requirement was easy to miss because it lives in a device implementer's head
rather than in the hub's code, so it is now **measured rather than assumed**:

```
timing
superframe 35, period 2000000 us nominal
beacon late: last 0 us, min 0, max 2 (spread 2)
```

**2 µs of spread against a 2 s superframe is about 1 ppm** — two orders of
magnitude below even an LSE-disciplined reference, so it contributes nothing.
Measured under sustained IPC load (84 round trips across a dozen boundaries), not
only on an idle core, because the risk is the beacon queueing behind other work.

It holds because the superloop polls the boundary directly and everything else it
does is short. It would stop holding if anything long-running were added between
polls — which is what the `timing` command exists to catch.

### The period is a contract constant, and a device must clamp to it

A device measures the period from consecutive beacons rather than being told it.
It must still **clamp the result to ±1% of `SUPERFRAME_US`** and reject a sample
outside that rather than adopting it.

Clamping makes the do-not-trust-the-hub property *stronger*. An unclamped
estimator can be walked anywhere its acceptance window allows — by a hostile hub,
or by a bench transmitter on the same sync word. A device whose estimate has
drifted far enough runs its counter ahead of the hub's, and then every honest
beacon arrives as a *backwards* move, which unsigned subtraction turns into an
enormous forward jump and a plausibility check refuses. The device goes deaf to a
hub that is transmitting perfectly. Found on the device side, where one bad first
sample was enough and the state was unrecoverable without a power cycle.

The original reason for discovering the period rather than declaring it was that
the hub's reference ran 3750 ppm fast, so the nominal was not real time. **That
reason is gone** — [the timebase is disciplined against LSE](timebase.md) and the
residual is −27 ppm. ±1% is ~100× the worst case two disciplined clocks produce
together, so it never bites an honest link.

Two specific poisoned samples become impossible: a period short enough to race a
device's counter ahead, and **4 s — which is exactly what one lost beacon looks
like**, and which sat inside the device's original 1–4 s acceptance window.

**Elapsed superframes come from the beacon's `superframe` field, never from the
number of beacons heard.** The hub deliberately omits beacons during a
[quiesce](pairing.md), and any beacon can be lost, so consecutive beacons are not
consecutive superframes.

### The superframe counter is the shared clock

It is carried in `radio_data_beacon_t` in `Common/inc/radio_protocol.h` — the
**shared** header, because a paired device takes its time alignment from this
frame and cannot see the hub's private ones.

That was not true until a sweep for "what does the new thing depend on that was
written under weaker assumptions" went looking. The frame was defined in
`CM4/Core/Inc/radio.h`, began with a broadcast address byte that filtered nothing
(`RFM69_FILTER_NONE`), named the counter `flags`, and carried the hub's raw TIM2
value as `clock` — a number that wraps every 71 minutes, restarts at reset and
means nothing off-board. Every one of those was correct while this was a hub-only
debug broadcast, and every one became wrong when a device had to decode it.

### The hub's reference was 0.4% fast — now corrected against LSE

One superframe is **2 000 000 nominal microseconds**. Taken as raw timer ticks
that was **1992.5 ms of real time**, not 2000 ms.

The cause is not the code. `TIM2` is configured for exactly 1 MHz — 200 MHz timer
clock, prescaler 199 — but the reference is wrong: `HSE` is 8 MHz taken from the
ST-Link MCO (`RCC_HSE_BYPASS`), not from a crystal. Measured against the host
clock over 597 s it runs at **1 003 878 Hz, +3878 ppm**.

**This is fixed.** The grid no longer steps 2 000 000 ticks; it steps
`timebase_us_to_ticks(2 000 000)`, using a scale measured against the LSE crystal
by TIM16 input capture. [timebase.md](timebase.md) is the whole of it — how the
measurement is taken, why it is averaged rather than tracked, and what about it
is still not understood.

Consequences the slot design has to absorb:

- **The superframe is a hub-defined tick, not 2000 ms of real time.** Devices must
  slave to the beacon rather than extrapolate from a nominal millisecond value.
- **A sleeping device pays for this in receive-window energy.** Every beacon
  re-aligns a device, so one in range never accumulates drift. A device that
  *sleeps* N superframes must open a window of roughly **±8N ms** to catch the
  next beacon — sleeping a minute means ±240 ms of receiver on-time to catch an
  8.5 ms beacon. The window is set almost entirely by the hub's reference; the
  device's TCXO contributes noise beside it.
- Every heard beacon re-aligns a device, so drift only accumulates across *missed*
  beacons. Sizing the guard is therefore about the maximum tolerated miss run, not
  about absolute accuracy.
- **The fix needed no hardware change** and is now in: the board's 32.768 kHz LSE
  crystal is fitted and oscillating, and TIM16 timestamps its edges with the same
  PLL1 tree TIM2 counts. An LSE is ~20 ppm against the MCO's ~4000, so the
  superframe is now real time to within the crystal, and the two bullets above
  are historical rather than current constraints.
- **A device could have compensated instead, and that would have been the wrong
  place.** Measuring beacon-to-beacon intervals and scaling slot offsets cancels
  a *static* hub offset without the hub changing anything. It fails exactly when
  a device most needs the grid — on first join, and after a run of missed
  beacons, when there is no recent interval to measure — and it asks all 64
  devices to work around one wrong clock.

Per superframe today:

- one data beacon, transmitted on the channel [the hop sequence](hopping.md)
  selects for that superframe counter;
- while a pairing window is open, a join beacon every **second** superframe on the
  fixed join channel — measured at 4008 ms spacing, exactly two superframes.

The superframe counter is the protocol's notion of time. It is broadcast in the
join beacon so a device is time-aligned before it has a key, and it is the index
into the hop sequence, which is what makes hopping stateless across sleep.

## Slot budget — why the clock comes first

**The reference accuracy decides how many devices fit.** Not throughput, not duty
cycle — the guard band, and the guard band is drift.

A device re-aligns on the beacon at the start of a superframe and transmits in
its slot up to ~2 s later, so it carries up to one superframe of relative drift.
Guard is that on both sides, plus about 1 ms of demodulation jitter and slot
quantisation. A sealed uplink frame — 12 B header, 16 B payload, 16 B tag — is
**17.6 ms** on air at 25 kbps.

| Reference | Drift over a superframe | Guard | Slot | Slots per superframe |
|---|---|---|---|---|
| ST-Link MCO, ~4000 ppm (was) | ±8.00 ms | 17.0 ms | 34.6 ms | **57** |
| LSE disciplined, ~20 ppm (now) | ±0.04 ms | 1.1 ms | 18.7 ms | **106** |

**57 was below the 64-device target**, with nothing left for downlink, retries or
a device that needs two slots. 106 leaves roughly half the superframe free.

That is why the clock came first: building the grid against the old reference
would have sized every constant against a guard about to shrink by a factor of
fifteen.

**The second row is now the one in force**, with one qualification — the ~20 ppm
is the crystal's own accuracy, and the hub reaches it by averaging a measurement
whose individual windows are far noisier than that. The averaging time constant
is ~32 s, so the figure holds in steady state and not across the first half
minute after reset. See [timebase.md](timebase.md).

## The slot grid

**Implemented as geometry**, in `Common/inc/radio_slots.h` — the shared header,
because a device computes its transmit moment from these offsets and shares none
of the hub's code. What is *not* built is the scheduler: nothing assigns a slot
to a device or serves an uplink yet.

| Region | Offset (µs) | Length | Contents |
|---|---|---|---|
| beacon | 0 | 25 000 | data beacon, 8.0 ms on air |
| downlink | 25 000 | 25 000 | one hub→device frame, at most every 2nd superframe |
| uplink | 50 000 | 1 824 000 | **96 slots × 19 000 µs** |
| join | 1 874 000 | 116 000 | join beacon + hub RX, only while a window is open |
| end guard | 1 990 000 | 10 000 | never scheduled |

Slot *N* opens at `50 000 + N × 19 000` µs after the boundary.

**Every offset is nominal microseconds.** The hub converts them through
`timebase_us_to_ticks()` because its own reference is not exact; a device slaves
to the beacon and needs no equivalent. Nothing in that header may be written in
timer ticks — a tick is a property of one board.

### Where 19 000 µs comes from

A sealed uplink frame is 12 B header + 16 B payload + 16 B tag = 44 B, plus the
11 bytes the modem sends around it (preamble 4, sync 4, length 1, CRC 2). At
25 kbps that is **17 600 µs**, and the guard is **1 400 µs**.

The guard is uncertainty and nothing else: relative drift across one superframe
(~40 ppm between two LSE-class references, 80 µs), preamble detection and
interrupt latency on a waking node, and slot quantisation. The device's
oscillator start is **not** in it — see the lead-time distinction below. That
separation was tested by events: the device's warm-up later fell from 10.4 ms to
2.4 ms and this number did not move, because the two budgets were never sharing.

96 slots against a 64-device target leaves room for a device that needs two, and
for retries.

### Duty cycle decides the downlink, not throughput

The hub's own transmissions must fit 1% of the superframe — **20 000 µs**.

| Frame | On air |
|---|---|
| data beacon (14 B) | 8 000 µs |
| downlink ack (28 B) | 12 480 µs |
| join beacon (14 B) | 8 000 µs |

All three every superframe is **28 480 µs — 1.42%, over the limit.** That is why
the downlink runs at half rate and the join beacon does too: 8 000 + 6 240 +
4 000 = 18 240 µs, **0.91%**. Idle, with no window open and nothing to send, the
hub sits at **0.40%**.

`Common/test/test_slots.c` asserts both directions — that the sustainable
combination fits and that the avoided one does not. Checking only that the budget
fits would let someone raise the rates and still pass, which is how the half-rate
decision becomes cargo cult.

There is still **no in-firmware duty-cycle governor**; the hub trusts the
schedule. Any change to these numbers has to be re-measured with the SDR.

### Planned: event windows, and why they are unacknowledged

Two kinds of uplink traffic are expected, and they want opposite things:

- **Regular status** — periodic, tolerant of a superframe of latency, and the
  right place for link telemetry (measured RSSI, a transmit sequence number).
  It goes in the device's assigned slot.
- **Irregular events** — rare, and they must arrive **as fast as possible**. In a
  pure grid an event waits up to a full superframe, 2 s, which is too slow for
  anything alarm-like.

The direction is a small number of **contention windows** carved from the spare
uplink slots — 96 exist against a 64-device target — where any device may
transmit an event immediately without waiting for its own slot. Every 12th slot
gives 8 windows about **228 ms** apart, an order of magnitude better than 2 s.

**Events will not be acknowledged.** Two independent measurements point the same
way, which is the reason to believe it:

- The hub's air budget is 20 ms per superframe and the beacon takes 8, so it can
  afford roughly **one urgent acknowledgement per superframe network-wide**.
- On the device, transmitting is cheap — 2.4 ms of oscillator wait plus air time
  — but *waiting for an ack* turns that into an open-ended receive window, and
  the device core is 99.8% busy while listening. It is the single most expensive
  thing a battery node can do.

So a blind retransmit on a later window costs the device two cheap transmits
instead of one transmit plus a receive, and costs the hub nothing. A retry one
window later adds 228 ms, still two orders below what a pure grid would cost.

**The open question, deferred deliberately:** without an ack, two colliding
devices both believe they succeeded. Whether that is acceptable depends entirely
on what the events *are* — a smoke alarm and a doorbell want opposite answers —
and **there is no latency or reliability requirement yet from either side**.
Recorded as an open question with a named cost rather than settled by inventing
a number, because a constant nobody can justify is worse than an absent one.

### Planned: what RSSI and a sequence number are actually for

**You cannot measure your own transmit path.** A device already measures the
beacon's RSSI — that is its downlink, and the hub need not tell it. The hub
already measures the uplink. Bytes on the wire exist only to tell the *other* end
what it cannot see about itself.

That makes the traffic asymmetric:

| | device → hub | hub → device |
|---|---|---|
| measured RSSI | yes | yes, but **rarely and per-device** |
| sequence number | yes | no |

RSSI **must not go in the beacon** — that is a broadcast, and
`radio_protocol.h` already records the rule that nothing per-device belongs
there, because a broadcast that grows with the device count does not scale. The
hub's report rides in the downlink slot instead, which is rare anyway.

One byte, **raw half-dBm**: `rfm69_get_rssi()` returns dBm×2 and the device's
SX126x uses the same convention, so there is no conversion and no rounding
disagreement between two implementations.

**A sequence number is mostly redundant in TDMA, and that is worth stating**
because the instinct comes from contention networks that have no shared
schedule. Here the schedule *is* the counter: missed downlinks are visible from
the superframe counter, and a missed uplink is an empty slot. What a counter
adds that the schedule cannot is the distinction between **"transmitted and
lost" and "never transmitted"** — for an event-driven sensor that is silent
until something happens, that is the difference between healthy and dead. It is
not needed in the downlink direction, where the hub detects its own loss from a
missing acknowledgement.

Both belong in the **encrypted payload, not the header**. RSSI leaks physical
position and a sequence number leaks activity pattern; the header is
authenticated but readable. It costs 2 of 16 payload bytes.

If one is added it must be named `tx_seq`, not `counter`. Two counters that look
alike and mean different things is the defect this project has already had once,
when a field called `flags` carried the superframe counter.

### What the grid does not have yet

- **Slot assignment.** The rule exists — assign from 0 upward, so the join region
  overlays unassigned tail slots — and it is a rule, not code.
- **Serving uplink.** The hub does not receive in slots yet.
- **A downlink queue.** The region is reserved and empty.

### Lead time and guard band are different budgets and must not share

- The device's **oscillator start is lead time**: deterministic and known in
  advance, so the node schedules around it — wake early, transmit on time. It
  constrains how late a node may *decide* to transmit and consumes no guard at
  all.
- **Drift is uncertainty**: neither size nor sign known in advance, so it is
  guard on *both* sides.

Spending the same milliseconds on both leaves the guard either oversized — paying
air time for nothing — or short of the uncertainty it has to absorb.

Device-side measurements, and they have already moved once:

| | was | now |
|---|---|---|
| RX entry latency | 10.4 ms | **2.4 ms** |
| wake lead before a slot | 18.5 ms | **under 12 ms** |

Most of the original figure was a configured TCXO wait taken from a reference
design — 640 steps, exactly 10 000 µs — not something the hardware imposed. The
device swept it 20× and the carrier's first-quarter frequency stayed flat to
60 Hz, 0.07 ppm, with no settling signature at any setting. The default is 2 ms,
keeping 4× margin over the shortest verified wait rather than taking the minimum.

**None of this changed the guard band, and none of it changed the grid.** It is
lead time, and the separation above is exactly why: the saving lands in the
device's sleep schedule, not in the hub's geometry. Nothing here reserved room
for the old number, so there was nothing to reclaim.

**Cold start is untested.** Oscillator settling is temperature dependent and the
sweep was at room temperature. If devices are specified below freezing this needs
re-measuring, not assuming.

Note that the wake lead is *not* air time: only the ~8.5 ms of transmission
counts against duty cycle. Summing warm-up into a duty-cycle budget is the same
class of mistake as the two already made here — a number that means something
other than what it looks like.

### Other constraints the grid absorbs

- **Crypto must not run inline.** A P-256 operation is tens of milliseconds —
  measured at **102.6 ms** on the device's PKA — and cannot happen inside a slot.
  This is why the asymmetric work sits on CM7
  ([ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md)) and why pairing gets its
  own [quiesce](pairing.md) rather than a slot. Per-frame AES-GCM is hardware and
  is fine inline.
- ~~The IPC mailbox has one slot and no sequence number.~~ Fixed —
  [ADR-0016](../decisions/0016-sequence-numbered-ipc-ring.md).

## See also

- [hopping.md](hopping.md) — the superframe counter's other job
- [joining.md](joining.md) — how a device acquires the counter
- [pairing.md](pairing.md) — the join region and the quiesce that suspends the grid
