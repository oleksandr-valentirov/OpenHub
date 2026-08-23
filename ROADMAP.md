# Roadmap

The single list of open work: debts, defects, and design that was agreed and
never built. **Nothing here is reasoning** — every item names the page in
`../radio_devices_docs` that holds the why, exactly as source comments do. If an
item needs a paragraph to justify it, that paragraph belongs on its page and the
line here shrinks to a pointer.

**Cite a tag or a commit message, never a bare SHA.** A rewrite orphans a hash
silently and neither the writer nor the reader is told.

**Cite this repository's own evidence, not another queue's item number.** A number
in a foreign queue moves when that queue is cleaned, and a number is not a
sentence, so nothing anywhere disagrees when it goes stale. Name the file, the
symbol, the commit or the ADR instead. The `device` items below are the exception
and are hints rather than identifiers.

**Cleaned 2026-08-23**: four closed entries retired, their reasoning left on
`open_hub/arch/build-and-generation.md`, `open_hub/arch/ipc.md` and
`open_hub/arch/keystore.md`; the front-end experiment the device session had been
carrying moved to `open_hub/radio/configuration.md`; and every item re-filed under
the heading its tag names.

An item leaves this file when it is done, not when it is understood. A defect
that turned out to matter for a reason worth remembering leaves a paragraph
behind on its page; the entry here just goes.

**Status words**

| | |
|---|---|
| `blocking` | the acceptance criteria cannot be met without it |
| `defect` | confirmed wrong, in the tree today |
| `debt` | agreed, costed, deliberately not built |
| `device` | the work is on the WL55 side; listed so the hub does not assume it |
| `contract` | changing it binds both firmwares — agree with the device session first |

Contract items also live in
[`radio/known-issues.md`](../radio_devices_docs/radio/known-issues.md), which the
WL55 repository reads and this file is not visible to. That page keeps the
reasoning; this one keeps the queue.

---

## The acceptance criteria

> An unpaired device listens. The hub sends it a pairing invitation. The device
> pairs with no side commands from a CLI, the two exchange keys, bring up an
> encrypted channel, and start exchanging regular messages. Both the device and
> the hub survive a reboot and restore the link. An event at a sensor reaches
> the hub and is processed there within 1 s.

Three readings were settled on 2026-08-21 and are recorded because the other
reading would change what gets built:

- **"No side commands from a CLI" means none typed on the device.** The
  operator's `device add <id> <pubkey>` on the hub is the out-of-band enrolment
  step and the criteria do not forbid it.
- **Slots are never chosen by an operator.** The hub assigns the base slot and
  the other two opportunities are derived by contract constants, so no operator
  picks any of the three.
- **Irregular messages are out and a deadline is in.** No contention windows and
  no transmit outside the grid; instead delivery *and* hub-side processing within
  1 s, which the grid as built does not yet meet.

Five of the nine clauses are done and verified on air, three are partial and one —
*both survive a reboot and restore the link* — has been exercised and failed. What
is left is below. The system-level statement of the same thing, with the evidence
behind each clause, is
[`specs/01-requirements.md`](../radio_devices_docs/specs/01-requirements.md) § 3.1.

---

## Blocking

### 1. Three opportunities a superframe, built and not yet on air — `blocking` `contract`

One opportunity a superframe made an event wait up to **2 s — twice the
deadline** — and the contention windows that were the plan are forbidden by the
same decision that set the deadline. **Three per superframe are the minimum**:
two bottom out at exactly 1000 ms, which fails for any positive air time.

Landed as `feat(grid): 50 kbps, three opportunities a superframe, and a duty
cycle that fits` with `feat(wire): link_v4` — 194 slots of 9400 µs, stride 65,
exactly 64 devices, the 1400 µs guard untouched, largest gap 778 ms and
210 750 µs left for the hub.

**What is left is the part no assert can do.** At 50 kbps two thirds of the
device's frames do not arrive (item 30), so the geometry that buys the deadline
has never delivered against it. A PER run at the new rate is the acceptance
evidence, not the host tests. Sensitivity is ~3 dB worse plus about a dB for the
h = 1 demod penalty, and neither side has read the datasheet row.

`radio/tdma.md` § the event deadline, § what k = 3 breaks.

### 3. The application payload has room and no application — `blocking`

The wire change landed: both sealed bodies are 16 bytes, frames are 39, and
`app_len` with `app[4]` uplink and `app[6]` downlink sit inside the air budget
the slot already paid for. `link_v5` pins both directions and both firmwares
compile the same digest.

**Nothing writes into it.** `app_len` is 0 on every frame, so "exchanging
messages" is still telemetry plus a command byte. This is an application question
now rather than a wire one, and the last blocking item that does not depend on
the radio.

After it the slot has **seven spare flag bits and nothing else**; the next byte
costs a grid change and a re-measurement.

`Common/inc/radio_protocol.h`, `radio/tdma.md` § slot budget.

### 4. The device has no reporting loop the hub can count — `blocking` `device`

The hub grants `report_every` and holds no evidence any device honours it. This
already produced one wrong dismissal: `uplink windows 375, sync 1` read as eleven
missing reports when the device had no reporting loop at all, so the population
was one transmission.

**The hub must not derive the denominator.** An `expected` column on `devices`
would print an assumption with a column heading. The device has to say what it
sent.

`open_hub/cli.md`, and the `verification` skill.

### 5. A device that loses the counter cannot find the hub — `blocking` `device`

The clause *both survive a reboot and restore the link* was exercised from the
hub side on 2026-08-21 and **the link did not come back**. Keys, slots and the
store all survived; the device's superframe counter did not, and the hop channel
is a function of it. `time follow` tunes for `superframe_now() + 1`, which after
a gap is the wrong number, so each attempt is a 1-in-28 draw.

**The fix is device-side and costs the hub nothing**: park on one hop channel and
wait, expected 56 s and 99 % within 254 s, then verify the counter heard maps to
the channel parked on. Listed here because ADR-0021 removed the broadcast join
beacon on an analysis that says this case does not exist.

`radio/joining.md` § re-acquisition after a counter loss.

### 6. The acceptance run has never happened as one sequence — `blocking`

Every part is verified separately and the whole has never run: clean hub, clean
device, enrol, invite, exchange, encrypted traffic, reboot both, recover.
Pairing has run **one device, once**.

Sweep the success-only paths while doing it. When the evening pairing first
worked, five defects surfaced in code that could only execute after something
finally went right.

`open_hub/testing/on-target.md`. Never erase bank 1 from CM7.

### 30. Two thirds of the device's frames never arrive, and the loss grows with offset into the superframe — `blocking` `defect`

Counted by both sides over one agreed superframe range, sf 647403..647694, with
the device transmitting 151 cycles on all three opportunities:

    slot   1    59 accepted of 151   39 %      (64 detected, 42 %)
    slot  66    22 accepted of 151   15 %      (24 detected, 16 %)
    slot 131     9 accepted of 151    6 %      (15 detected, 10 %)

**23 % detected, 20 % accepted**, at −17 dBm — the level both sides had been
calling healthy. A 4x cadence change does not move it, so there is no receiver
ceiling and no cadence effect. The hub cannot compute the denominator (item 4):
`accepted / delivered` is 81 % while `delivered / transmitted` is 31 %, and only
the numerator exists on this side.

**Decomposed 2026-08-22: the loss is entirely before sync.** `sync_match` equals
`frames + crc_err` exactly, so nothing is lost after detection, and the CRC
failure *rate* is flat across k while the counts are not. Level (sd 0.7 dB), AFC
and LNA gain are flat across k as well, and the receive window is open from
50 000 to 1 873 600 µs, so no window closes between opportunities. Item 37's
`worst lag 658 us` bounds the superloop three orders of magnitude below the
611 ms between opportunities, which kills FIFO blocking as the mechanism.

**This item shares its whole signature with 60 and 63**, which is why the three are
one knot rather than three investigations: pre-sync in all of them, device to hub in
all of them, one receiver in common. An arm run against any of them is evidence
about the other two. `../radio_devices_docs/specs/03-roadmap.md` § K2.

**What is left is placement.** Per-cycle slopes of arrival against k change sign
between cycles (+480, −107, +357 …), so this is per-cycle placement noise rather
than a constant clock-scale difference, and the statistic to grade is the
*spread* of those slopes. Neither side's placement instrument can see the class
alone: each measures its own frames against its own computed target.

**The discriminator is running.** Window 2, cut at superframe 713752, separates
the two boards by `SUPERFRAME_PERIOD_BASELINE` (1 against 64) at a matched
received level — treatment and control split by **board rather than by time**,
which is what every earlier window lacked. Grading is pre-registered: the spread
of per-cycle slopes and the k-gradient between boards, reported as *not measured*
if either board reaches fewer than 8 cycles.

`radio/phy.md`, `open_hub/radio/configuration.md`.

### 23. The hub's channel filter has never had more than 1 kHz of margin — `blocking` `contract`

**Retitled 2026-08-23.** This item was about the *device's* filter being 5 kHz too
narrow. The device's is still short and still loses nothing; what changed is that
the hub's own filter turns out to be the one with no room, and it is the receiver
on the losing side of items 30, 60 and 63.

`rfm69_rx_bandwidth_to_reg` picks **the narrowest encodable setting at least as
wide as it is asked for**, and this project has always asked for
`RADIO_RX_BW_MIN_HZ`. So the margin is whatever the encoding rounds up by, and
nothing has ever chosen it:

    regime                      asked      encoded    slack
    25 kbps, RegRxBw 0x8A      99 000     100 000    1 000 Hz
    50 kbps, RegRxBw 0x82     124 000     125 000    1 000 Hz

Both regimes, one kilohertz, by accident of the encoder's step. Against that, the
carrier error the slack is supposed to cover:

    RADIO_CARRIER_ERR_HZ                   12 000    the allowance
    hub RegFei, max over arrivals          12 329    already past it
    device afc, max over arrivals          19 287    item 63's arms
    required at 19 287                    138 574    13 574 Hz more than the hub has

**The population is censored and that is the whole caveat**: those are the frames
whose sync word matched. What a missing frame needed is not in the sample and
cannot be, so this bounds nothing — it establishes only that the allowance is
being spent by the frames that *succeeded*.

The next encodable step is **166.7 kHz**, which covers 33 333 Hz of carrier error
for **0.97 dB** of noise bandwidth against roughly 50 dB of margin at bench range.
`rfm69_set_rx_bandwidth_hz` writes `RegAfcBw` to the same value, so widening moves
the AFC's acquisition filter with the channel filter — an AFC asked to pull in
19 kHz through a filter sized for 12 is the mechanism, and one write changes both.

**Two things argue against, and both belong in the pre-registration.**

The cross-direction reading: the device's filter is **7 358 Hz short** of the same
budget and lost 0 of 19 beacons, while the hub's clears it by 342 Hz and loses four
frames in five. Both receivers face the same relative offset by symmetry — one
crystal pair, one difference — so **filter width alone cannot produce that
asymmetry.** Either something else is also wrong, or one of these numbers does not
mean what it is being read to mean.

Which is the second: **nobody has checked whether `RegRxBw` is a single-sideband
or a double-sided figure.** Every margin above assumes double-sided. Nothing in the
driver, the skill, `radio_phy.h` or `radio_devices_docs/radio/` says which, and the
answer moves all of it by a factor of two. Settle it with a sweep — narrow the
setting until reception breaks and read the breakpoint against a known offset —
not by reading a datasheet sentence about a different part.

**The allowance itself contradicts its own design page** and that is an ADR, not an
experiment: `phy.md` says the modulation tolerates ±20 ppm at both ends, which is
40 ppm relative and **34 660 Hz** at 866.5 MHz, against a constant of 12 000 —
13.8 ppm. Three times over, since the constant was written, sizing both filters.
166.7 kHz does not reach the page's figure either; it is 2.7 kHz short of it. The
next free ADR number is in the workspace `CLAUDE.md` and nowhere else.

The constants are named apart, `RADIO_RX_BW_HUB_HZ` and `RADIO_RX_BW_DEV_HZ`,
against a shared `RADIO_RX_BW_MIN_HZ`. **Only the hub's is asserted**, because
asserting the device's fails both builds today and the configuration change is
the device's to make. The device cannot follow the hub's number in any case: the
SX126x table steps 117 300 to 156 200.

`radio_devices_docs/radio/phy.md`, `radio_devices_docs/specs/03-roadmap.md` § phase 1.

### 59. The pairing exchange is twice the join region it runs in — `blocking` `contract`

One exchange holds the join channel for **~270 000 us** end to end, and
`RADIO_JOIN_LEN_US` is 116 000 with `RADIO_JOIN_RX_US` at 100 000. Only 116 400 us
remain in the superframe after `RADIO_JOIN_OFFSET_US` and the end guard, so the
exchange is **2.2x the region and 2.2x all the room there is**.

Measured 2026-08-23 by the device, all three spans from one clock: invitation to
request 43 772 us, request to response **108 275 us**, response to confirmation
96 068 us. An earlier estimate of 214 300 counted the curve and not the machinery
and was 20% low.

~37 200 us of the hub's 108 275 is neither air nor arithmetic - it is the FIFO
read, the IPC to CM7 and `pairTask` being scheduled. Item 40 measured 45 ms of
mailbox on a different path, which is the second witness.

It shows as `req 7 -> rsp 7 -> conf 3 -> accept 3`: every request answered, four
of seven exchanges dying between the response and the confirmation, and a fresh
enrolment succeeding 1 of 5 once any device is already paired against 1 of 1 when
none is.

`begin_quiesce()` is the mechanism meant to buy the air and cannot: a quiesce
starts at the **next** superframe boundary and the exchange starts 126 ms before
one, so the response goes out before the clear air begins. `RADIO_QUIESCE_MIN_GAP`
then refuses back-to-back enrolments outright — `pairings that got no clear air`.

**A was withdrawn on measurement, both halves.** The quiesce is already armed at
the request and already takes effect at the boundary the exchange's tail lives
in; `quiesce_lost` was 1 of 7, so six exchanges had their clear air and failed
anyway. And `RADIO_QUIESCE_MIN_GAP` is enforced by the device too, so relaxing it
here would suspend a grid the devices still believed was running. One real defect
came out of A and is fixed: a valid PAIR_REQ was discarded when `begin_quiesce()`
refused, which made that attempt certain to fail rather than merely unlikely.

**This item is a margin, not the cause of the current failures.** Counted on both
ends over one closed window: device sent 4 requests, hub saw 3; hub answered 3,
device heard 2; device confirmed 2, heard 1 accept - loss on all three legs with
**zero CRC errors**, which is a receiver not listening rather than a damaged
link. Item 60 carries that.

B moves `RADIO_JOIN_OFFSET_US` to 1 718 000 and spends 16 uplink slots.
C - the 37 200 us of mailbox - is worth doing anyway and cannot fix this alone.
The decision record it needed is
[ADR-0026](../radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md),
accepted 2026-08-23 and built on both this side and the device; what it did not
close is the pre-sync loss, which is why items 60 and 63 outlived it.

`../radio_devices_docs/radio/pairing.md` § the exchange no longer fits the region.

### 60. The hub registers a PAIR_CONF about two times in five - `blocking` `defect`

**This item said "has never registered one". That was wrong**, and it was wrong
in the way a small sample is: the claim rested on `conf 0` after one or two
confirmations had been radiated, which at the rate measured since is a coin
landing the same way twice. Ten controlled enrolments on 2026-08-23, each from an
erased store and a fresh device identity:

| | device radiated | hub registered |
|---|---|---|
| confirmations | 12 | **5** |
| enrolments completed | - | **5 of 10** |

Against the WL55-to-WL55 control's 4 of 4 the difference is suggestive and not
established: Fisher one-sided **p = 0.13**. What *is* established is that the
fault is **intermittent**, and nothing about it changed between the runs that
produced "never" and these - no receive-path code has moved.

**The confirmation usually produces no sync word at all.** That is the finding
that replaces "lost below the parser", which came from the one window that
happened to hold a CRC error. In the failing trials the hub's window counters
read `sync 1, frames 1` or `sync 2, frames 2` - one sync per *request*, and none
for the confirmation that followed. When it is heard it arrives at **-42 to -43
dBm**, the same level as the request beside it.

So the receiver is not listening when the confirmation lands, and this is not a
link, level, CRC or FIFO problem. Where to look, in order:

- `join_rx_deadline` is `rfm_micros() + RADIO_JOIN_RX_US`, **100 000 us**, set
  when the window opens. The device's own response-to-confirmation turnaround is
  **105 000 us**. The second half of the exchange cannot fit the window it starts
  in, and `join_region_service` puts the part in standby the moment the deadline
  passes, whatever the exchange is doing.
- `frame_tx` returns the part to RX after transmitting the response, so the hub
  does keep listening past the deadline - until the next `join_region_service`
  pass closes the window underneath it. Which of the two happens first is a race,
  and a race is what a rate near half looks like.
- With `device_count == 0` the window instead runs to the superframe boundary
  less the end guard, so the same exchange meets a **60 ms** blind gap plus a
  beacon transmission if it crosses a boundary.

The fix is not a longer constant: it is that an exchange in flight must hold the
window open until it resolves or times out.

Two things this rules out, both measured: the front end (item 59's page carries
the -42 dBm figure and the SX126x's -44 dBm on the same frames) and the budget.

`../radio_devices_docs/radio/pairing.md` § the WL55-to-WL55 control.

### 63. The hub misses a PAIR_REQ that reached its antenna — `blocking` `defect`

Item 60's twin and a separate leg: the confirmation is lost while the hub
**waits**, this one while the hub has just transmitted the invitation and
returned to receive. Item 59 named it and item 60 carried it, whose title is
about PAIR_CONF, so it had no entry of its own until now.

Six trials on 2026-08-23 under ADR-0026, cumulative `join reqs seen` differenced
per trial against the device's own `req sent`:

| trial | device sent | hub registered | sync | crc err | frames |
|---|---|---|---|---|---|
| 1 | 3 | 1 | 1 | 0 | 1 |
| 2 | 3 | 1 | 2 | 0 | 2 |
| 3 | 1 | 1 | 2 | 0 | 2 |
| 4 | 1 | 1 | 2 | 0 | 2 |
| 5 | 2 | 2 | 4 | 1 | 3 |
| 6 | 4 | **0** | 1 | 1 | 0 |

**14 radiated, 6 registered.** Retries covered it in four trials and not in the
sixth, which is why that trial's exchange is excluded from ADR-0026's denominator.

`sync == frames + crc_err` in **all six**, so nothing is lost between the sync
detector and the parser, and `dropped 0` says nothing was refused above it. The
loss is entirely that no sync word matched.

The device placed every request correctly: `invite -> request 43 880 us` in the
failing trial against 43 864..43 880 in the successful ones.

**The receiver was on.** A `RegOpMode` read off the part at
`RADIO_PAIR_INIT` air + `RADIO_PAIR_REQ_LEAD_US` into the region, once per join
window. Over both arms below it found the receiver out of RX in **0 of 902**
windows and in **0 of 210** that carried an invitation. Mutation-proven: built
against `RFM69_MODE_SLEEP` the same counter reads 30 of 30 the other way, so a
zero from it means something. "The window never opened" is refuted, not merely
unobserved.

Thirty-six further trials on 2026-08-23, in two pre-registered arms:

| | requests radiated | registered | enrolments |
|---|---|---|---|
| A — as built | 69 | **34** | 6 of 20 |
| B — the join window's RSSI sampler suppressed | 15 | **7** | 4 of 6 |

**p = 0.54.** The sampler was the leading hypothesis and it is dead: the hub
triggers `RssiStart` on every superloop pass and spends 500 us of each ~630 us
pass timing out, so the part is inside a manual measurement about 80% of the time
and a 1.28 ms preamble cannot avoid one. Removing it changed nothing. The AFC
spread, pre-registered as the second measure, did not narrow either.

Arm B is six trials and not the ten pre-registered, because the roster cache filled
mid-batch. Its four remaining trials are void and are not counted.

**What stopped it was the roster cache, not a broken store**, and it is repaired:
dropping tombstones returned 63 of the 64 entries and the boot scan reports 0
errors. What does not change is the shape — the log still reclaims nothing, so a
batch that draws a fresh identity per trial **spends an entry it never gets back**,
and **this experiment's design is what exhausted the instrument.** The rule that
follows is `release` rather than an erase, so a node keeps its id and re-enrols
under an entry the cache already holds. ADR-0027 is what removes the ceiling.

**What the arms did not kill is the carrier.** `device afc` over each arm:

| arm | frames | min | max | last |
|---|---|---|---|---|
| A | 48 | -12 330 | **19 287** | 13 061 |
| B | 15 | -10 132 | **17 089** | 8 666 |

`RADIO_CARRIER_ERR_HZ` is **12 000** and both arms exceed it — in the frames that
*arrived*. The lost ones are not in that sample, so it bounds nothing on its own,
but the allowance is demonstrably being spent. The untried arm is
`RADIO_RX_BW_HUB_HZ`. **Item 23 carries that arm**, its arithmetic and the two
readings that argue against it; it is not restated here.

`../radio_devices_docs/radio/pairing.md` § the request that reached the antenna.

### 67. CM4's wait on CM7 refreshes now; the control has not run — `blocking` `defect`

**Fixed in code 2026-08-23, unverified on hardware.** `CM4/Core/Src/main.c` armed
IWDG2 at 512 ms — as short as ~348 ms with the LSI at a tolerance nobody has read
off the datasheet — and then spun on `HSEM_ID_0` with no refresh in the loop,
while CM7 releases that semaphore in `StartDefaultTask`, after `osKernelStart()`
and `MX_LWIP_Init()`.

**This is what bricked the board twice.** An erase of a bank 1 sector takes 954 ms;
held before the release it outlasts IWDG2, the system resets, the erase is cut
mid-flight, and the sector is left raising `SNECCERR1` and `DBECCERR1` — which
`ks_init()` reads at every subsequent boot, hard-faulting a board with a perfectly
good image. It never presented as a timeout.

Both halves are built:

- The wait moved to `CM4/Core/Src/bootwait.c` and refreshes **unconditionally**
  rather than paced off `HAL_GetTick()`, because pacing would make the safety
  property depend on SysTick still running — the same class of fault. The tick
  measures and nothing else.
- The budget is declared. `Common/inc/hub_boot.h` carries
  `HUB_CM7_BOOT_BUDGET_MS`, both cores compile it, `bootwait_ms()` reaches the
  console through `ipc_timing_t`, and `timing` prints the measurement beside the
  budget and flags an overrun. How long CM7's boot took was not observable from
  anywhere before this.

**What is owed is the control, and it is two flashes.** Nothing yet proves the
refresh does anything: on a fast boot the loop body may run few enough times that
a build without it would behave identically. Mutation in both directions is the
check — remove the refresh, confirm the board resets before CM4 reaches
`RFM_Init`; restore it, confirm it does not. A plain reset with no erase in flight
is harmless, so this costs nothing but a flash. Until it runs, this item is a
change that compiles.

The first `timing` after a flash also gives the number the budget should be
tightened against; 3000 ms is a declaration awaiting its first measurement.

`../radio_devices_docs/open_hub/arch/dual-core.md` § the wait between step 3 and
step 4, § the budget is declared.
### 68. Six devices share one uplink slot on flash — `blocking` `defect`

Exposed 2026-08-23 by fixing the roster cache, and it was there the whole time.

`lowest_free_slot()` picks the lowest slot no **cached** live device holds. While
the cache was full of tombstones the live devices were not in it, so every
enrolment saw one device at slot 0 and answered **slot 1**. Six of them did:

```
b7e33ff6    0 enrolled
3cfde754    1 enrolled
527b51e7    1 enrolled
48e4c4fb    1 enrolled
22cdec51    1 enrolled      <- node A's identity
a18892ec    1 enrolled
3c7a11d9    1 enrolled
```

**The slot is written into the record**, so this is on flash and permanent for
those records. It is not a display artefact.

**Two things were wrong at once and only one of them is fixed.** `device list`
reported `1 enrolled` while the store held seven, because the cache had no room
for them. That cache defect is fixed — `scan()` no longer holds an entry for a
device whose newest record is a removal, and reads the older sector first so scan
order is seq order. The colliding slots are the damage that happened while it was
open, and they are on flash.

**It matters to CM4, not only to tidiness.** `install_device()` does
`d = &devices[k->slot]`, so all six map to `devices[1]` and overwrite one another.
Any pairing that lands on one of them serves a roster entry another device also
believes is its own.

None of the six carries a key — all read `(no key yet)` — so nothing is paired and
nothing is lost by removing them. The repair is `device remove` on all seven and
`device add` for whichever are wanted, which now assigns slots against a cache
that holds the whole roster. It costs one flash slot per removal out of 1731.

`../radio_devices_docs/open_hub/arch/keystore.md`.

### 12. The link fails at high input level, and the mechanism is not settled — `blocking` `defect`

Found 2026-08-21 by the device session dropping its transmit power from +14 to
−17 dBm. Matched windows attributed frame by frame off two independently computed
hop maps:

    A  -17   57 frames   8 syncs   8 accepted   0 CRC failures
    B  +14   36 frames   1 sync    0 accepted   1 CRC failure
    C  -17   36 frames   9 syncs   8 accepted   1 CRC failure

    accepted  B 0/36 vs C 8/36   p = 0.0025
    pooled    low 16/17 syncs accepted vs high 2/20   p = 1.8e-07

C reproduced A on fresh channels minutes later, so drift is excluded. **Which end
is not settled**: the experiment varied transmit power, which is this receiver's
front end in compression *and* that transmitter's PA at its rated maximum,
perfectly confounded, and both predict every observation.

**This receiver is cleared at −40 dBm and nowhere else.** The LNA ladder pinned
`LnaGainSelect` G1..G6 — thirty decibels — against a level the transmitters held
fixed:

    pinned   G1     G2     G3     G4     G6      A crc      B crc
    board A  -40.7  -42.2  -42.8  -43.1  -40.2   6/7 5/5 8/10 11/12 7/12
    board B  -39.2  -41.7  -42.2  -42.1  -39.3   2/5 2/3 5/6  5/7   6/9

**2.4 dB of movement for 30 dB of gain, and not monotone**: `RegRssiValue` is
input-referred, and the `rfm69` skill's contrary claim is corrected. The pin is
real — frames arrive at every step and board A's CRC rate falls to 7/12 at G6
against 11/12 at G4 — so this is not a command that did nothing. A front end in
compression comes out of it somewhere in 30 dB; at −40 dBm this one is linear.

**The ladder ran at −40 because the board had already been turned down, and
−25 dBm, where the missing decibels live, was never tested** — narrowed by the
device session. The direction runs against this side: a compressed reading reads
*low*, so the true step would be nearer the 23 dB requested, which explains the
discrepancy with no defect on the transmitter at all.

Also established: `lna_gain` reads **G1 on all 120 rows** with the level column
known good (31 dB of transmit power asked, 31 dB delivered), so the AGC is not
backing off under the strongest signal this bench has produced. Within one board
the loud end is not yet shown to cost frames — 33/50 at −48 dBm against 2/6 at
−25 is p = 0.18.

**Owed, and it costs three minutes: run the ladder again at −25 dBm.** It needs a
board back at +14, which breaks whatever window is open, so it is announced first
and run between runs. Any repeat must **log the intervention** and equalise the
*received* levels rather than the transmitted ones — equal transmit power leaves
23 dB of siting difference in place, which is the confound itself.

Not measured by the same ladder, though it was meant to come free: the hub's
sync-detection slope against level, +1.74 ± 2.42 µs/dB on one board and
+0.05 ± 1.63 on the other, intervals containing zero and the device's −3.09. The
pre-registered power calculation used the spread at one opportunity (sd 40-54 µs)
while the run collects all three (236 µs) — **power has to be computed on the
population the run will have.**

`radio/phy.md`, the `rfm69` skill.

---

## Defects

### 7. A join beacon still shares the first invitation's superframe — `defect`

`PAIR_INIT` targets round up to `RADIO_PAIR_INIT_EVERY` (4) and the join beacon
runs every `JOIN_BEACON_EVERY` (2), so **every** invitation lands on a beacon
superframe. Both are keyed at `join_offset_tk`, about 8 ms apart.

**Half guarded, and the unguarded half is the case that matters.** In
`join_region_service()` the paired branch now refuses the beacon when
`pi_superframe == frame_counter` or the region is owned by an exchange. The
`device_count == 0` branch — a hub that has never paired anything, which *is* the
first-pairing case — instead defers the beacon as `join_beacon_pending`, and that
pending flag is cleared only by `pair_region_owned()`, which is false until a
request has already arrived. So on the invitation superframe itself the beacon
still goes out.

By name the fault ADR-0021 records: the device heard 15 beacons and no invitations
until the beacon was suppressed. **Found by reading, not on air**, so what it costs
a receiver is still unmeasured.

`CM4/Core/Src/radio.c` -> `join_region_service`, the `device_count == 0` branch.

### 8. The LSE measurement is unexplained at the window level — `defect`

The mean is sound — it matched a host-clock measurement to 51 ppm over ten
minutes — but a single 7.8 ms window carries ~350 ppm of noise that a sixteenfold
longer window barely reduced. That should be impossible: the accumulated span
telescopes to two timestamps. Averaged over ~32 s it does not block the grid.
`RCC_BDCR.LSEDRV` is at its lowest reset default and is the untested lever.

`open_hub/radio/timebase.md`.

### 14. `rssi_up` is read from an untriggered latch — `defect`

`handle_uplink_frame()` takes the level with `rfm69_get_rssi()`, which reads
`RegRssiValue` and triggers nothing, so the sealed report carries a level with no
provenance.

**Half done, and the half that is done answers what the entry asks.**
`sync_rssi_sample()` samples at the sync-match edge and `device afcraw` prints it
per frame: **−43 dBm against a −96 dBm floor**, 53 dB apart, so the latch is not
holding a between-frames sample. The first version triggered a fresh measurement
instead and failed on every frame — **an RSSI trigger does not complete while
`SyncAddressMatch` is high** — which is why the counter counts attempts and
failures apart rather than successes alone.

What remains is `rssi_up` itself. `afc_note()` consumes the sync-match sample
before the frame handler runs, so the fix is to stash it for the frame just
delivered rather than to read again.

`CM4/Core/Src/radio.c:1378`, `:1625`, `open_hub/radio/configuration.md`.

### 31. The two sides' boundary lag disagree in a direction that cannot happen — `contract` `defect`

Neither `BEACON_BOUNDARY_LAG_US` nor `UPLINK_AIM_US` appears anywhere in
`radio_devices_docs/radio/`. **Silence cannot go stale**, which is why the
contract page never disagreed with either side and nobody found this by reading.

The hub measures superframe boundary to first bit directly: **358..366 µs over
529 beacons**. The device compiles **260 ± 5 µs**, applied as
`at_us - BEACON_BOUNDARY_LAG_US` where `start_us` has already subtracted
`RADIO_PRE_SYNC_US`. So the device's constant covers the hub's term **plus** its
own detect-to-timestamp residual, and a residual cannot be negative — **the
device's number must be larger than the hub's and it is 100 µs smaller.**

**Adopting either number would be the worst outcome available.** The discriminator
is agreed and cheap: the device forges a beacon on its own boundary while the hub
reports the lag it computes, to be run in the same boot as item 30's window so it
does not become another two-window fraction.

The pre-sync term is the suspect this side can contribute to: `RADIO_PRE_SYNC_AIR_US`
halved with the rate, 2 560 µs to 1 280, so any lag reading taken before the rate
change and compared with one after is out by a preamble and a sync word.

Raised by the device session. Device items 9 and 12.

### 37. The sync-RSSI window is fixed, and the instrument is still blind one way — `defect`

`SYNC_RSSI_WINDOW_US` gated `sync_rssi_have` at 8000 µs from the sync edge where
the frame ends at 6720, so samples in the gap were the noise floor admitted as a
frame level. Fixed by `fix(cm4): the sync-RSSI window was measured from the wrong
instant`, and read on 2026-08-22:

    levels: 179 tried, 0 late, 0 failed, worst lag 658 us

No level in `afcraw` is a floor reading wearing a frame's name, and the
superloop's period — listed as unmeasured everywhere it mattered — is bounded
above by that 658 µs.

**What remains is the direction that would hurt.** The instrument records the lag
of samples that were *taken*; a stall long enough to lose a frame produces no
sample, so `0 late` is a statement about arrivals and not about the superloop's
worst case.

`open_hub/radio/configuration.md`.

### 50. Nothing refuses to run on an undisciplined timebase — `defect`

`calib_ready()` has **no caller outside `CM4/Core/Src/calib.c`**, and
`CM4/Core/Src/timebase.c` starts at the nominal `scale_q24 = 1u << 24`. So a hub
whose LSE never delivers a window runs the TDMA grid on a scale that is
measurably wrong and says so to nobody.

Measurably is the word — the disciplined scale on this bench sits **3452 ppm**
off nominal over 2082 accepted windows:

    2 s superframe   x 3452 ppm = 6.9 ms of placement error
    RADIO_SLOT_US 9400 us        guard 1400 us

Three ways in, none loud: `calib_init()` returns after `FIRST_WINDOW_US`;
`started` stays 0 if `span_max > htim16.Init.Period` or `HAL_TIM_IC_Start()`
fails; and `ready` is **sticky**, so one window at boot and a dead crystal
afterwards leaves it 1 forever. `calib_age_tk()` computes the number that would
name all three and **nothing acts on it** — the decorative shape.

The fix is not refusing to boot: a hub that cannot discipline its timebase can
still pair, answer a server and be debugged. What it cannot do is hold a grid, so
the refusal belongs where the grid starts, and the condition has to leave
northbound under its own name.

`open_hub/radio/timebase.md`.

### 64. The join region's level instrument almost never completes — `defect`

`rfm69_measure_rssi` triggers and waits `RSSI_TIMEOUT_US` = 500 us for RssiDone.
Measured 2026-08-23 over 31 join windows with nothing on air: **76 successes from
about 95 000 calls**, and **0 from 534** inside the span where a request's payload
would be. Both are the same 0.08%; the span is not special.

So every `rx level: peak/floor` this project has quoted for the join region rests
on about 75 samples, which is why peak reads -84 to -85 dBm in every trial
including the ones that paired. **It measures the band, not the frame**, and it
cannot say at what level a missed request arrived.

The part evaluates RSSI continuously while the receiver waits for a preamble, so
a manual trigger is mostly ignored — `OpenHub/.claude/skills/rfm69` already says
the receiver "parks until a signal arrives". Until this is fixed the level witness
below the sync word is the SDR, not the hub.

`../radio_devices_docs/radio/pairing.md` § the request that reached the antenna.

---

## Debts

### 11. A zero timebase scale would wedge the grid, and only luck forbids it — `debt`

`superframe_due()` in `CM4/Core/Src/radio.c` steps the boundary by
`superframe_tk` in a `while` loop, so a zero step is an unterminated loop with a
runaway counter, and `timebase_ticks_to_us` divides by the same scale. The device
session hit exactly this on 2026-08-21.

It cannot fire here, and **the reason is two derivations away from the value**:
`calib.c` rejects any capture window outside ±2 % of nominal. Nothing between
`timebase_set_scale`, which takes any `uint32_t`, and the loop that trusts it
would notice a zero. The fix is to make the bad value unrepresentable at the
setter rather than checked at the use.

`open_hub/radio/timebase.md`, and the `verification` skill § failure-path sweep.

### 9. Every timing figure assumes HSE is the ST-Link MCO — `debt`

X3 is unfitted. If an HSE crystal is ever soldered on, re-measure — the method
needs no instruments, just `timing` and a host clock.

`open_hub/radio/timebase.md`.

### 10. No in-firmware duty-cycle governor — `debt` `contract`

The hub trusts its schedule rather than counting its own air time, so any
slot-timing change must be re-measured with the SDR.

**k = 3 promotes this from tidiness.** A device using all three opportunities
every superframe sits at **1.200 %** at 50 kbps, so the deadline caps the
sustained event rate as well as costing capacity — and nothing in either firmware
would refuse, every individual frame being legal. The device's bound is a budget
over the regulator's hour (36 s of air), not an integer per superframe.

The SDR cannot referee it either: `--expect-ms` selects our bursts by air time,
and halving the frame while tripling the count invalidates that selection, so the
instrument needs re-validating at the chosen rate before it can gate anything.

`RADIO_DUTY_PPM` now derives the figures from `RADIO_UPLINK_AIR_US`, because a
budget held in frames is a budget a wire change edits silently.

`radio/phy.md` § duty cycle, § the device's budget.

### 33. Half rate is no longer forced by duty cycle — `debt` `contract`

`RADIO_DOWNLINK_EVERY` is 2 because beacon + downlink + join beacon every
superframe came to over 1 % at 25 kbps. At 50 kbps the three are **16.0 ms,
0.800 %**, and fit.

**Nothing is wrong today** — the value stands and its assert passes. What changed
is that it is now a choice rather than a requirement, so a proposal to raise the
downlink rate has to be refused or accepted on its own merits. Listed so the next
reader does not re-derive the old refusal from a page that no longer says it.

`radio/phy.md` § duty cycle.

### 36. The downlink nonce guard has never refused anything — `debt`

`dl_nonce_is_new()` is the per-device floor that makes the downlink's nonce
uniqueness explicit instead of a side effect of `dl_served`'s scheduling. No path
reaches it today, so it is a guard for the day that scheduling variable is
removed as the optimisation it resembles.

**A counter that has only ever read 0 is indistinguishable from one that cannot
read anything else.** `device dlnonce` is the control shipped with it: it asks
the live predicate for its verdict at the last sealed superframe, one past it and
one before, and the three must read 0/1/0. Unread until a boot.

`radio/crypto/wire-crypto.md`.

### 40. A snapshot costs 45 ms, almost all of it waiting on the mailbox — `debt`

`snapshot_us` reads 45 000 with two devices installed. The work is nine
`hub_ipc_call()` round trips and each polls for its reply on a 5 ms `osDelay`, so
the figure is the polling interval times the number of calls and has almost
nothing to do with CM4. At 64 devices it would be over half a second of one task
spinning.

The fix is a reply notification rather than a poll, which the doorbell semaphore
already does for events. It becomes urgent at the first bench with more than
about ten devices.

`open_hub/arch/ipc.md`, `open_hub/network/telemetry.md`.

### 49. A frame-ring row cannot say which superframe or which regime it came from — `debt`

`ipc_afc_raw_t` carries `grid`, `slot`, `gain`, `rssi` and `afc` per sample and
**no superframe**, so a northbound row cannot be joined against the device's own
transmit log by arrival. Packing the counter at snapshot time does not fix it:
that number is when CM7 assembled the snapshot.

`grid` is a partial witness already on the wire — it is a bijection onto
`superframe mod 28`, so it can **refuse** a claimed superframe with no new field.
Not built. Reconstruction cannot be more than a refusal: over 22 observed
inter-arrival gaps, **13 exceeded the hop cycle**, so a guess would be right 41 %
of the time and silently wrong the rest.

The honest fix fits: `IPC_PAYLOAD_MAX` is 96 and `sizeof(ipc_afc_raw_t)` is 72,
so a `uint16_t` per entry costs 18 of the 24 free bytes with CM7 restoring the
high bits. A CM4-to-CM7 contract change, so both cores reflash together.

**The same gap in a bigger form:** one afternoon put the bench through four
regimes — a device firmware change, the transmit power it restored, the level
match that undid it, and two hub flashes — and every boundary falls *inside* the
ring. Both sessions had to partition from memory, and twice that memory was
wrong. Time is the wrong key; `hello.build` and `boot_id` already exist and are
sent once per connection, so what is missing is carrying them **per row**.

**`boot_id` is now carried per row, by the server rather than by the hub.**
`openhub-server` stamps each ring row with the connection's `boot_id` and keys
deduplication on `(boot, seq)` — its commit `fix(state): seq is per hub run, and
the ring outlives the run`. `tx_seq` restarts at reset and that ring does not, so
until then every row of a new run was discarded as a repeat. `build` is not
stamped, the superframe above is untouched, and no reader outside that server
gets any of the three.

`open_hub/arch/ipc.md`, `open_hub/network/telemetry.md`.

### 57. Duty cycle cannot be attributed to one transmitter — `debt`

`dutycycle.py` scans the whole captured band, and the 1% limit is **per
transmitter**. A wideband capture holds the hub, both devices and the foreign
traffic seen on grid channels 9, 10, 16, 17 and 18, so its total is a ceiling for
any one of them and cannot be compared against a per-transmitter prediction.

Everything needed already exists elsewhere: `airgrid.py` classifies each burst by
grid position — beacon, downlink, `uplink<n>` — and C6 already measures mean air
time per position against `phy.air_us(payload)`. What is missing is summing that
per transmitter over the capture duration.

Until it exists, the regression's duty-cycle check runs on a single-transmitter
capture with the other side holding transmit, which spends a bench agreement on
something arithmetic should cover.

`specs/06-regression.md` §6.1.

### 61. Enrolment mode SECRET has no MAC primitive any more — `debt`

`crypto_pair_init_mac` was HMAC-SHA256/96 over the invitation's cleartext, keyed
by a K_init that `crypto_pair_init_key` derived from Z1. ADR-0024 replaced that
with mode OPEN and an all-zero MAC, and the keying half went with it. The MAC
half stayed: declared, defined, **called by nothing and covered by no test**, with
a comment still citing ADR-0021. It was removed on 2026-08-23.

`RADIO_ENROL_MODE_SECRET` is still reserved in the protocol header. Building it
needs a MAC keyed by a **provisioned** secret, which is a different key from the
one the deleted function took, so this is a note about what is owed rather than
about a function to restore.

Half a mechanism is worse than none: the surviving half took a 32-byte key with
no derivation left to produce it, and the next reader would have keyed it with
whatever was to hand.

`../radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md`.

### 62. The frame cipher's self-test is unreadable exactly when it matters — `debt`

`aead_selftest` is reported by one line inside `devices`, printed **only when it
is nonzero**, and placed after the per-device list. With eighteen records enrolled
the CLI's response buffer truncated the output four lines earlier, so the result
was not absent — it was unreachable, and silence was indistinguishable from a
pass, from a truncation and from a command that never ran.

Found on 2026-08-23 while confirming the self-test still passed after it was
repointed from pair_v2's vectors to pair_v4's. Emptying the keystore made it
readable, which is the wrong way round: a keystore with devices in it is the
normal case.

It wants its own line, printed pass or fail, ahead of anything unbounded.

`../radio_devices_docs/open_hub/security/self-tests.md`.

### 69. Two failure renderers give distinct words and nothing tests that they do — `debt`

`ks_fail_str()` and `hub_ipc_str()` exist because four store conditions shared one
number and two IPC failures shared another. Both now name their case — and **a
build that returned one string for every code would print a plausible sentence and
pass every check there is.**

Neither body needs the HAL or CMSIS; the files they sit in do. Split them into
pure translation units and one host test covers both. It is the cheap half of the
lesson the store cost a day for: an instrument that cannot separate its own cases
belongs with the other prerequisites, not after them.

`../radio_devices_docs/open_hub/arch/ipc.md`,
`../radio_devices_docs/open_hub/arch/keystore.md`.

---

## Contract debts

### 15. The data beacon is unauthenticated — `contract`

So the quiesce flag is a denial-of-service primitive. Bounded by a clamp, a rate
limit, and the fact that a forger must hold the hop key to be on the right
channel — **none of which is a cryptographic guarantee**. The fix is a network
broadcast key, which has the property such keys always have: one compromised node
can forge broadcasts. Worth making deliberately rather than inheriting.

Unlike the join beacon, which is unauthenticated *by necessity*, this one is
unauthenticated *because nothing has sealed it yet*.

`radio/known-issues.md`, `radio/pairing.md` § denial of service.

### 16. The hop key is a placeholder until a device pairs — `contract`

It lives in CM7's flash, so for ~3 s after a hub reset CM4 hops under a
placeholder derived from `hub_id` while CM7 replays its store. A paired device
misses a beacon or two across a reboot. A property of the contract's bootstrap
rather than of either implementation; listed so nobody diagnoses it as a fault.

`radio/known-issues.md`.

### 17. GCM partial final word, on both sides — `contract`

`HAL_CRYP_Decrypt` does not mask the unused bytes of a partial final word while
encrypt does, so every length not a multiple of four fails with byte-perfect
ciphertext. Mitigated by zeroing the input buffer before the copy, and pinned by
the 19-byte grant and the 23-byte `wire_v3` case. **Not fixed — contained**: a
new code path that forgets to zero reintroduces it.

`radio/known-issues.md`.

### 18. Key rotation: generation 0 only — `contract`

The agreed base case is `R(0) = Z`, so generation 0 is already `pair_v2`'s pinned
session key and no vector moves when the ratchet is written. The hub deliberately
stores **no** ratchet state rather than a value from the scheme that was not
chosen. The hop key has no rotation path at all, and that is chosen:
`PAIR_ACCEPT` is the only sealed downlink that exists, so changing it means
re-pairing every device.

`radio/crypto/key-lifecycle.md`.

### 19. Two clauses of ADR-0021 were agreed and never built — `contract`

- **`PAIR_REQ` still carries a public key and is still 56 bytes**, not 24. ADR-0025
  took the field from `pubkey[33]` to `pubkey[32]` when X25519 replaced P-256, so
  the frame is one byte shorter than when this entry was written and is nowhere
  near what the decision asked for. The size is asserted in `radio_protocol.h`
  twice, against `sizeof(radio_pair_req_t) == 56` and against
  `RADIO_PAIR_REQ_BYTES`. Buys about 10 ms of device air for a coordinated wire
  change.
- **The broadcast join beacon is still transmitted.** The decision removes it; the
  implementation only suppresses it on superframes an exchange owns — and item 7 is
  that even this is incomplete.

`radio/joining.md` § what ADR-0021 left unbuilt.

### 21. `UPLINK_AIM_US` is a contract number that lives on one side — `contract`

Where a frame sits inside its slot is the same kind of number as where the slot
sits inside the superframe, and it belongs in `Common/inc/radio_slots.h`. It is
700 µs on the device — the frame is centred in the slot's slack — and appears
nowhere in the hub, which assumed flush at slot start. Both sides stayed
internally consistent while computing about different geometry, and it has
already cost one wrong guard analysis.

**Measured from the hub side 2026-08-22 on the current grid.** `arrival_sync_us`
is the DIO3 edge against `superframe_start_tk`, so it carries no processing term
at all:

    0xc4d444aa   aim 659 / 616 / 604
    0xdcbac6f5   aim 574 / 545

Flush at slot start would predict 1 280 µs of residual and the readings are 1 825
to 1 939, so **the aim is real and the hub's old assumption is the side that was
wrong.** Both boards land 40 to 155 µs *early* of the 700 they compile. The
device session's own instrument — sharing no code, no clock and not even the same
side of the antenna — gives 634 sd 4 and 586 sd 5 over 114 transmissions each,
agreeing per board to 3 and 12 µs and **reproducing the difference between the
boards** (48 µs their way, 66 µs this way).

Their side has the cause: `UPLINK_LEAD_US` models the ramp and the `SetTx` delay
about 100 µs longer than the real path. **Early eats the guard from the safe
side**, which is why nothing ever failed and why no counter could see it — the
hub's uplink window is open across all three opportunities, so an early frame is
received and recorded nowhere as early.

**The number does not go in the shared header until it is a population.** Five
readings is not one; the target is ~20 per board per opportunity off
`/api/stream`, and what lands in `Common/inc/radio_slots.h` is then agreed with
the device session rather than taken here.

`verification` skill § know which artifact each assert pins.

### 22. `uptime_s` is trusted on arithmetic and unconfirmed on a board — `contract`

**The comment is corrected and the field is believed.** It said the field wraps
at 4294.967 s; it does not. The device session pinned it across the crossing —
4294 before, **4296 after**, 11496 two hours past — and the correction is in
`Common/inc/radio_protocol.h` as of `link_v5`.

**Neither side's field data ever reached the test.** The hub's largest
observation is 1496 s and the device's 4255 s, against a 4295 s threshold. A host
test proves the arithmetic, not the board.

What it unblocks is also unbuilt: `devices` prints `never` for a device that has
not reported, covering *gone*, *deaf* and *serving a nonce reservation* (item 29)
with one word. `uptime_s` splits them and nothing on this side reads it that way.

`radio/known-issues.md`.

### 29. `never` renders three different device states with one word — `contract`

**Rescoped 2026-08-23: the device's half is gone.**
[ADR-0023](../radio_devices_docs/radio/decisions/0023-the-hub-supplies-the-transmit-floor.md)
removed the durable counter reservation, so a device that keeps its counter is no
longer silent for up to 33 minutes after a reset — an opened downlink supplies its
transmit floor and the hub needed no change at all for it.

What remains is this side's, and it was always the harder half. `devices` shows
**`never`** for a device that has not reported, and that one word covers a device
that is *gone*, one that is *deaf*, and one that is *between resets and has not yet
opened a downlink* — the hub being the only place an operator looks. `uptime_s`
splits them and nothing here reads it that way (item 22).

`radio/known-issues.md`, `open_hub/network/telemetry.md`.

### 55. A device setting changed without a reset has no witness — `contract`

`radio power -9` was sent to a board on 2026-08-22 and left **nothing on the
wire**. It was found from the other side of the antenna, as a 15 dB step in the
received level, and only because someone was watching for one.

    boundary                      witness           state
    hub reset                     boot_id           carried, now per row
    device reset or reflash       dev_uptime_s      on the wire, unused
    a setting changed in place    none              needs a field from the device

It matters because it is the boundary most likely to be crossed **during** a
measurement rather than between two: a reset announces itself by breaking the
link, a console command does not.

The device reporting its own transmit power in the sealed report closes it, and
that is a contract change — agree it with the device session, whose item 52
covers the setting not persisting rather than not being reported.

`radio/known-issues.md`, `open_hub/network/telemetry.md`.

---

## Design agreed but unbuilt

### 25. RSSI and a sequence number in the sealed payload — `debt` `contract`

Costed and designed, not written. Both belong in the **encrypted payload, not the
header**: RSSI leaks physical position and a sequence number leaks activity
pattern, and the header is authenticated but readable. Two of sixteen payload
bytes. The hub's RSSI report rides the downlink, never the beacon. If a counter
is added it must be named `tx_seq`, not `counter`.

Blocked behind item 3, which is the same wire change.

`radio/tdma.md` § planned RSSI and a sequence number.

### 26. TLS on the northbound interface — `debt`

Nothing is implemented, and there is now a link to protect: telemetry and
commands run in the clear over TCP between the hub and `openhub-server`, so
anything on the LAN can read every device's RSSI and issue `device_remove`.

The protocol is OHT over one outbound connection, so the hub is a TLS **client**
and mbedTLS goes straight over the socket — no `altcp_tls`. What is left is
identity: PSK is far smaller on the hub, a pinned self-signed certificate is
trivial on the server and costs the hub X.509 and ECDSA. The PSK branch was
blocked on the stdlib — `set_psk_server_callback` is absent on the 3.12 the
server's Dockerfile pins and was added in 3.13, measured here only as absent on
3.12.3. **Verify by raising the base image, not by reading this line.**

One fact belongs beside this item rather than inside it: the REST and websocket
API carries **no authentication at all** — the token is checked in HELLO and
nowhere else — so the `device_remove` exposure is reachable there too. Turning
authentication on breaks every caller at once, which is why it is a decision and
not a patch.

`open_hub/network/tls.md`, `open_hub/network/telemetry.md`.

### 38. The northbound link forgets its server on every reset — `debt`

`telem server <ip> <port> [token]` has to be retyped after a reset, exactly like
`ip static`. Both wait on a configuration store; the `cfg` command that stubbed it
out was removed on 2026-08-22, so nothing on the console advertises it any more.
Until the store is built the link cannot come up unattended, which is most of what
a server is for.

**The store is now specified rather than absent.**
[ADR-0027](../radio_devices_docs/open_hub/decisions/0027-config-store-is-a-ring-of-checkpoints.md)
is accepted and unbuilt: a journal of fixed-size typed records wrapping between two
sectors, with periodic checkpoints and small deltas between them. The one thing
that gated it is measured — CM7 erases a bank 1 sector in 954 ms, from ITCM, with
no error bit, **once `HSEM_ID_0` is released first** (item 67). Building it closes
this item, retires the 64-id ceiling behind item 68, and is the largest single
piece of unbuilt design in this queue.

`open_hub/network/telemetry.md`, `open_hub/network/ethernet.md`.

### 39. A device command still cannot say anything the wire has no word for — `debt` `contract`

`dev_app` reaches the firmware end to end and is refused there with
`not_implemented`: the downlink's `app[6]` is unwritten, which is *item 3* seen
from the north side. Everything above the radio is built, so agreeing the payload
with the WL55 session is the only work left and it lands as one commit in
`radio.c`.

`open_hub/network/telemetry.md`, `radio/tdma.md` § slot budget.

---

## Bench debts

### 27. Cold start is untested — `device`

Oscillator settling is temperature dependent and the sweep was at room
temperature. If devices are specified below freezing this needs re-measuring,
not assuming.

`radio/tdma.md` § lead time.

### 28. Owed to the device session

- **The LNA ladder re-run at −25 dBm** (item 12), announced before it starts.
- Node B's PA-ramp capture.
- Node A (`0xC4D444AA`) has never been resynced.
- A constant for radiated energy in the duty-cycle model — **model only, never a
  roll-back**, so it must not be wired to anything that suppresses a transmit.
- A bracketed `timing` / `syncstats` / `timing` read over a named superframe
  window, so their `no-beacon` count has `beacon_n` as a denominator.
- **Warning before the next hub reset**, so node B is parked and instrumented
  when it happens. Park-and-wait recovery is built on their side; an unannounced
  reset spends the one honest test of it on nobody watching.

`open_hub/testing/sdr.md`.
