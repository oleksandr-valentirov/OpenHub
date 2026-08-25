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

**Cleaned four times.** The first pass retired four closed entries, moved the front-end
experiment the device session had been carrying to
`open_hub/radio/configuration.md`, and re-filed every item under the heading its
tag names. The second retired **six** — the carrier arm, CM4's watchdog refresh,
the store's reader/writer split, the mailbox flood, the device's own send count,
and the boot erase — leaving their reasoning on `radio/phy.md`,
`open_hub/arch/dual-core.md`, `open_hub/arch/config-store.md`,
`open_hub/arch/ipc.md` and the `verification` skill. The third retired item 81,
the hop deck stage: verified on the H755 with two swapped deck bytes as its
control on 2026-08-24, reasoning left on `radio/hopping.md` and
`open_hub/security/crypto-architecture.md`. The fourth retired item 80, the
crypto backend: it was closed on 2026-08-25 by phase 9 steps 4 and 5 and left
here wearing a `closed` tag, which is a closed item in the costume of a status
column. Its reasoning is on `radio/phy-seam.md` § the crypto seam does not exist,
and the record of what the built seam narrowed to is
[ADR-0031](../radio_devices_docs/radio/decisions/0031-the-link-layer-is-a-rule-with-two-roles-and-the-session-layer-stays-on-cm7.md).
The fifth retired item 11, the zero timebase scale: its loop half closed when the
stepping rule moved to `radio_stack/src/grid.c`, and its division half closed
2026-08-25 with a guard at the setter and `CM4/test/test_timebase.c` to make that
guard red. Reasoning on `open_hub/radio/timebase.md`; what the work turned up
about `ipc_timing_t`'s headroom is item 87 above, because it is a different
problem that happened to be in the way.

**Nothing was added for the `.gitignore` defect found in the same hour**, and
that is deliberate. `CM4/test/Makefile` was absent from `HEAD`, so a clone could
not run RG-H-9 at all; it is tracked now, and a fixed defect does not get a queue
entry to commemorate itself. The rule the next harness needs is on
`open_hub/testing/host-tests.md` § one of these suites was not in the repository,
which is where somebody adding one will be.

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

**Measured 2026-08-24, and item 30's figure reproduces.** Two 600 s windows, the
denominator taken off the device rather than derived here: 38 sent and 13
accepted, then 38 sent and 13 accepted. 47 of 76 never reached sync. The
deadline is therefore met by the *grid* and missed by the *link*, and the two
numbers must not be quoted together — a 778 ms worst-case gap over a path that
drops three frames in five is not a 778 ms delivery.

`radio/tdma.md` § the event deadline, § what k = 3 breaks.

### 3. The application payload has room and no application — `blocking`

The wire change landed: both sealed bodies are 16 bytes, frames are 39, and
`app_len` with `app[4]` uplink and `app[6]` downlink sit inside the air budget
the slot already paid for. `link_v6` pins both directions and both firmwares
compile the same digest.

**Nothing writes into it.** `app_len` is 0 on every frame, so "exchanging
messages" is still telemetry plus a command byte. This is an application question
now rather than a wire one, and the last blocking item that does not depend on
the radio.

**`link_v6` halved the uplink's half of it**, to `app[2]`, to make room for
`temp_c_x10` — a first-class device measurement rather than an application one.
The downlink's `app[6]` is untouched. That was the cheaper of the two ways to
find two bytes: the other was 320 µs per slot across 194 slots and a re-measured
grid.

After it the slot has **four spare flag bits and nothing else**; the next byte
costs a grid change and a re-measurement.

`Common/inc/radio_protocol.h`, `radio/tdma.md` § slot budget.

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
ceiling and no cadence effect. When this was written the hub could not compute
the denominator at all — `accepted / delivered` was 81 % while
`delivered / transmitted` was 31 %, and only the numerator existed on this side.
**The device counts its own sends now**, so the population comes off the far side
of the antenna: two 600 s windows, 38 sent and 13 accepted in each.

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

**Two devices at once, 2026-08-24, run `2026-08-24-4`, both denominators off the
nodes:**

| | the node sent | the hub accepted | delivered | bad |
|---|---|---|---|---|
| node A, slot 1, −44 dBm up | **42** | 12 | **29 %** | 0 |
| node B, slot 0, −55 dBm up | **47** | 14 | **30 %** | 0 |

Both at `report every 8` and `k = 0`, so this is the single-opportunity figure and
not comparable to the 151-cycle k=3 rows above. `frames_bad 0` on both: the loss
is still entirely before sync. **Eleven decibels between the two boards moves the
delivered fraction by one point**, which is another arm against a level or
sensitivity ceiling and was taken without arranging anything - the two boards
simply sit at different distances.

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


#### 2026-08-24: 0 of 2, and the mechanism above is pre-ADR-0026

Two exchanges got three frames deep in `bench/runs/2026-08-24-2/`, which is the
first time a `PAIR_RSP` and a `PAIR_CONF` have been seen on air at all. Four
instruments on the confirm leg:

- **the air:** `PAIR_CONF`, 26 bytes, correct ids, and then nothing until the
  next scheduled beacon ~4 s later. No `PAIR_ACCEPT` was ever radiated.
- **the device:** `conf sent 2`, `accept heard 0, timeout 2`, every refusal
  counter on the accept path zero — it is not rejecting a grant, it is not
  getting one.
- **the hub's ladder:** `req 2 -> rsp 2 -> conf 0 -> accept 0, 0 paired`, with
  `timed out 2`. The exchange sat in `RADIO_EX_SENT_RSP` until CM7's timeout.
- **the hub's radio:** `sync 2, crc err 0, frames 2` across the whole window, and
  those two frames are the two requests. Neither confirm became a payload.

**The "where to look" list above describes the pre-ADR-0026 geometry and has to be
re-derived.** It compares a 105 000 µs response-to-confirmation turnaround against
a 100 000 µs window, which was the arithmetic when the whole exchange ran inside
one join region. Under ADR-0026 it does not: the device now confirms
**2 007 970 µs** after the response, in its own region, and the hub reports
entering 30 join regions over the window. The old race may be gone and the symptom
is not, so what closes the window under an exchange in flight is an open question
again rather than a diagnosis.

**Device item 61 is the sharpest instance of this leg and it has a known cause.**
The WL55 hub role loses the confirmation **11 times out of 11, deterministically**,
because `hublogic.c` arms `HUB_EX_TIMEOUT_US` from the response and knows nothing
of `RADIO_PAIR_CONF_REGION`. That is a third image nobody counted when ADR-0026
landed, and it is worth reading before hunting this one on the H755 — the same
shape of miss on a second implementation is the cheapest hypothesis available.

`../radio_devices_docs/radio/pairing.md` § the WL55-to-WL55 control.

**Measured again 2026-08-24, run `2026-08-24-4`, and the fraction is literal.**
Two enrolments back to back on a cleared roster: `req 5 -> rsp 5 -> conf 2 ->
accept 2, 2 paired`. **Two confirmations of five**, in one window rather than
pooled across sessions.

What is new is that both sides counted every leg and they reconcile with nothing
left over:

| leg | direction | sent | received |
|---|---|---|---|
| PAIR_REQ | device → hub | 7 | **5** |
| PAIR_RSP | hub → device | 5 | 5 |
| PAIR_CONF | device → hub | 5 | **2** |
| PAIR_ACCEPT | hub → device | 2 | 2 |

**Downlink 7 of 7, uplink 7 of 12**, and each device's timed-out leg is the far
side's missing frame one for one. So this is not the hub failing to *register* a
confirmation that arrived — it is the confirmation not arriving, and the same
receiver loses requests in the same window at the same rate. **This item and 63
are one mechanism seen at two frame types**, which the pooled figures could never
show. `bench/runs/2026-08-24-4/RESULTS.md`.

**A mechanism found on the other hub and ruled out here.** The WL55 hub role lost
38 confirmations of 38 to its own join beacon transmitted on top of them
(`wl55_device/ROADMAP.md` item 61, run `2026-08-24-3`). This hub does not have
that defect - `pair_region_owned()` at `CM4/Core/Src/radio.c:1906` is exactly the
guard the other role was missing - so the two-in-five is a different cause and
that arm is closed rather than open.

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
but the allowance is demonstrably being spent.

**The filter arm was the obvious next one and it has been run, negatively.**
`RegRxBw` is a single-sideband figure, so the hub's filter is ±125 kHz with
**75 000 Hz** of carrier-error room rather than the 1 000 Hz that made it the
suspect — measured by holding a transmitter still and stepping the filter down
until reception stopped. It cannot be what loses these requests.
`radio_devices_docs/radio/phy.md` carries the sweep. What survives of the carrier
hypothesis is item 73, which is arithmetic rather than a mechanism.


#### 2026-08-24: the first air-side denominator, and proximity ruled out

Every arm above differenced two counters. `bench/runs/2026-08-24-1/join.iq` and
`2026-08-24-2/` count the requests **on the air** instead, on a bladeRF over the
join channel, so the denominator is no longer the device's own claim.

| window | requests radiated | hub registered |
|---|---|---|
| 2026-08-24-1, boards adjacent | 8 | **0** |
| 2026-08-24-2 window 1, boards apart | 4 | **2** |
| 2026-08-24-2 window 2, boards apart | 4 | **0** |

**A nearby transmitter desensitising the receiver was the standing hypothesis and
it is dead.** 2 of 8 against 0 of 8 is Fisher **p = 0.47**. The first window alone
reads 2 of 4, p = 0.09, and quoting it would have closed this item as fixed —
**one window is not a measurement here**, which is why both are in the table.

Two things the air added that no counter could. The hub's own `pair_init` counter
and the capture agree to the frame on the invitation leg — 14 built, 14 sent, 14
on air — which is the control that says the capture saw everything radiated.
And the device's `invites seen 4 of 7` is **its listening duty cycle, not path
loss**: its retry gap is ~8.5 s against an 8.0 s invitation period, so it answers
every other invitation, and it answered every one it was awake for.

Level, for the record, from the same hub in the same minutes: node A's uplinks
read **-44/-48 dBm** and decode 11 of 11; node B's requests read **-56/-57 dBm**
and decode 2 of 8. Both far above the hub's reported floor of -91 dBm.

**A counting hole on this path.** After `pair_reqs_seen++`, every early return
increments `pair_reqs_dropped` **except** a failed
`ipc_send_event(IPC_EVT_PAIR_REQ, ...)`, which does `ex_reset(); return;` and
counts nothing. Not the cause of any zero above — those never reached the counter
— but a blind spot on the path being debugged.

`../radio_devices_docs/radio/pairing.md` § the request that reached the antenna.

**2 of 7 in run `2026-08-24-4`**, with the device count as the denominator: two
nodes sent seven requests between them and the hub saw five. In the same window
it saw two confirmations of five. **The rate is the same order at both frame
types and the direction is the same**, which is the argument for reading this and
item 60 as one receiver rather than two defects. `bench/runs/2026-08-24-4/RESULTS.md`.

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

### 86. `device add` refuses the one command that re-pairs, and offers one that cannot — `defect`

**Two commands, and the CLI has them the wrong way round.** `cfg_enrol()` is
idempotent by design: on a known id it keeps the slot, bumps `key_gen`, wipes
`root_key` and `session_key`, and returns OK — that **is** the re-pair. And
`device add` is the only path that reaches `RFM_open_pairing()`, whose single
caller is `IPC_REQ_ADD_DEVICE`, so it is the only command that opens **CM4's**
ears.

The CLI refuses it on a paired device — *"would discard its session key"* — and
points at `device window`, which calls `pairing_arm_init()` and arms **CM7's
invitation sender** alone. So invitations go on air while CM4 drops every
`PAIR_REQ` answering them, and the operator sees a window that is open and a
device that will not pair.

**Cost, measured 2026-08-25:** six enrolment windows in a batch executed neither
command and read as six failed pairings. `device remove` then `device add` is
the way through and is what the bench procedure does now.

The guard is not wrong to exist — discarding a working session key by accident
is worth refusing. What is wrong is that the refusal names a replacement that
does not do the job. Either `device window` opens CM4's ears too, or the message
says `device remove` first. **Agree with the server session**: its enrolment UI
offers `pair_window` for the same reason.

`radio_devices_docs/open_hub/radio/pairing.md`, `bench/journal/2026-08-25-architect.md`.


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


#### 2026-08-24: the first air-side denominator, and proximity ruled out

Every arm above differenced two counters. `bench/runs/2026-08-24-1/join.iq` and
`2026-08-24-2/` count the requests **on the air** instead, on a bladeRF over the
join channel, so the denominator is no longer the device's own claim.

| window | requests radiated | hub registered |
|---|---|---|
| 2026-08-24-1, boards adjacent | 8 | **0** |
| 2026-08-24-2 window 1, boards apart | 4 | **2** |
| 2026-08-24-2 window 2, boards apart | 4 | **0** |

**A nearby transmitter desensitising the receiver was the standing hypothesis and
it is dead.** 2 of 8 against 0 of 8 is Fisher **p = 0.47**. The first window alone
reads 2 of 4, p = 0.09, and quoting it would have closed this item as fixed —
**one window is not a measurement here**, which is why both are in the table.

Two things the air added that no counter could. The hub's own `pair_init` counter
and the capture agree to the frame on the invitation leg — 14 built, 14 sent, 14
on air — which is the control that says the capture saw everything radiated.
And the device's `invites seen 4 of 7` is **its listening duty cycle, not path
loss**: its retry gap is ~8.5 s against an 8.0 s invitation period, so it answers
every other invitation, and it answered every one it was awake for.

Level, for the record, from the same hub in the same minutes: node A's uplinks
read **-44/-48 dBm** and decode 11 of 11; node B's requests read **-56/-57 dBm**
and decode 2 of 8. Both far above the hub's reported floor of -91 dBm.

**A counting hole on this path.** After `pair_reqs_seen++`, every early return
increments `pair_reqs_dropped` **except** a failed
`ipc_send_event(IPC_EVT_PAIR_REQ, ...)`, which does `ex_reset(); return;` and
counts nothing. Not the cause of any zero above — those never reached the counter
— but a blind spot on the path being debugged.

`../radio_devices_docs/radio/pairing.md` § the request that reached the antenna.

---

## Debts

### 88. `airgrid.py` counts bursts that are not on the grid, then accuses the firmware — `defect`

Regression `2026-08-25-1`, RG-A-3. C5 reported *an uplink sat on a channel the
hub did not name* and named **channel −6**, which is not a channel: it is
864.50 MHz, below the band. The burst was classified as an uplink **by its
duration** — 9.22 ms against the uplink's 8.70 — which is the failure the
`verification` skill already records as *a control classified by the quantity it
measures is not a control*.

**Two instruments disagreeing is what found it.** `hops.py` reads the same
bursts, marks them `!` and warns *21 burst(s) outside the 29-channel grid*;
`bandscan.py` calls channel 30 *BEYOND FILTER — not evidence*. `airgrid.py`
folds them into C5's uplink set and into C1's denominator.

**C1's threshold failure is entirely this.** 30 beacons + 15 downlinks + 4
uplinks = 49 ours, 22 on no grid position, and 49/71 = **69.0 %** — the exact
figure C1 reported against its 70 % bound. The foreign bursts sit at 15.5–21 dB
against our 74–76 dB, so a level gate as well as a channel gate would refuse them.

The fix is a channel gate before classification, not a threshold change. A per-
position statistic on this bench has to be robust to the transmitter `RESOURCES.md`
records under `air:tx`, and this one is not.

`../radio_devices_docs/open_hub/testing/sdr.md`,
`../bench/runs/2026-08-25-1/RESULT.md`.

### 89. The SDR tools cannot read the capture the regression plan asks for — `defect`

`specs/06-regression.md` tier 2 says `-s 4e6 -t 300`. That is **4.8 GB**, and
`airgrid.py` loads it whole: `np.fromfile` to float32 is 9.6 GB and the complex
view another 9.6 GB. The kernel killed it at **26.9 GB resident on a 30 GB
machine** — `Out of memory: Killed process`, from the kernel log rather than
inferred.

**No tool in `tools/sdr/` takes an offset or a duration**, so there is no way to
grade a long capture except by cutting a slice beside it with `dd`, which is what
run `2026-08-25-1` did. Every earlier run that was actually graded used ~60 s.

So the plan's own line has never been run end to end on this bench, and the two
halves of one code block disagree: the capture command produces a file the
analysis commands cannot open. Either the tools take a window or the line does —
and a windowed tool is the better answer, because it also makes a long capture
gradeable in pieces rather than discarded.

`../radio_devices_docs/specs/06-regression.md` § tier 2.

### 87. `ipc_timing_t` is full at 96 of 96, so the next field silently does not fit — `debt`

Measured 2026-08-25, not recalled: `sizeof(ipc_timing_t)` is **96** and
`IPC_PAYLOAD_MAX` is **96**. The `_Static_assert` beside it passes at exact
equality, which is correct and is also the point at which it stops carrying
information — the next reading CM4 wants to send CM7 does not fit, and what a
reader sees is a failed assert rather than a reason.

**It has already cost one instrument a reader.** `timebase_scale_refused()`
counts scale writes refused as unrepresentable; the host suite reads it and
nothing on the board does, because the `timing` line would need a field here.
That is left as a tripwire with a host-side consumer rather than paid for with a
field somewhere else, and the reasoning is on
`open_hub/radio/timebase.md` § the refusal has no board-side reader.

Two ways out and neither is free: raise `IPC_PAYLOAD_MAX`, which touches the ring
both cores compile and every other payload's headroom; or split the struct, which
costs a second request type and a second round trip on a path that already costs
45 ms (item 40). **Worth deciding before the next field, not after the assert
fails.**

`../radio_devices_docs/open_hub/arch/ipc.md`.

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

### 73. `RADIO_RX_BW_MIN_HZ` doubles a single-sided requirement — `contract`

    #define RADIO_RX_BW_MIN_HZ  (2u * (RADIO_DEVIATION_HZ + RADIO_BITRATE_BPS / 2u + \
                                       RADIO_CARRIER_ERR_HZ))

`RegRxBw` was measured to be single-sided, so the doubling asks the hub for
twice the width the part needs, and `_Static_assert(RADIO_RX_BW_HUB_HZ >
RADIO_RX_BW_MIN_HZ)` has been passing for the wrong reason.

**It is not one edit.** The macro is in `Common/inc`, the device's
`RADIO_RX_BW_DEV_HZ` is an SX126x number and that table *is* double-sideband, so
one expression cannot be right for both sides at once. The two constants need
separate minima named for which convention each part uses, which is a contract
change and belongs to both sessions.

Nothing is broken today: the hub runs 125 000 Hz single-sided against a need of
about 50 000, which is why this is a debt and not a defect. It is filed so that
the next person to compute a margin from this macro does not compute it twice.

`radio_devices_docs/radio/phy.md`.

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
`Common/inc/radio_protocol.h` as of `link_v6`.

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

### 90. The region has to leave CM7 — `debt` `contract`

[ADR-0031](../radio_devices_docs/radio/decisions/0031-the-link-layer-is-a-rule-with-two-roles-and-the-session-layer-stays-on-cm7.md)
decision 8: on the H755 the band is CM4's and CM7 compiles none of it. Today both
cores define `RADIO_PROFILE` from independent literals in their own
`CMakeLists.txt` and nothing on the board compares them.

CM7's own half is small and is not blocked on the library. Measured 2026-08-25 by
removing the define from `CM7/CMakeLists.txt` and building: CM7 fails in **four**
places, and only one is this repository's — `CM7/Core/Src/cli.c:2100`, the third
line of `rxbw`, which prints the compiled receive bandwidth beside the `asked_hz`
CM4 has just returned from the same header. It is a number re-derived on the core
that does not own it, so the line goes rather than moves.

The other three are `radio_stack/inc/radio_slots.h` and are `../radio_stack/ROADMAP.md`.
`RADIO_HUB_HANDLE_SLACK_US` is CM7's one genuinely rate-derived symbol, used once;
it is a cross-core budget and belongs in the mailbox.

**The exit is that CM7 builds with `RADIO_PROFILE` undefined**, checked by
removing it — not by reading this line.


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

### 38b. The report rate is in the store and still not read from it — `debt`

**What is left of item 38, which is closed.** `telem server`, `ip static` and
`ip dhcp` now apply and persist through `hubconfig`, and
`hubconfig_apply_boot()` replays them after `MX_LWIP_Init()`. Measured: the
threshold set by `devices lost 3` came back as 3 across a reset, and a stored
server address is applied before the telemetry task first dials.

`cfg_config_t.report_every` is the one field of the head nothing writes and
nothing reads. It is not the same shape of work as the rest were: the rate has to
reach CM4 as well, so the seam would need an IPC call it does not have — `devices
rate <n>` sends `IPC_REQ_SET_REPORT_RATE` from `cli.c`, where `rfm_request` is
static. Either the request moves somewhere both can reach, or `hubconfig` gains a
callback the CLI installs.

Until then the granted rate is `RADIO_REPORT_EVERY_DEFAULT` at every boot, which
is correct and not what the operator last chose.

`open_hub/arch/config-store.md` § 8c, `open_hub/network/telemetry.md`.

### 75. `radio.c` is the chip and the protocol in one file — `debt`

**Step 1 of the cut is done: the PHY contract is one file.**
It holds the eight calls and the four-kind event, and
`wl55_device/Core/Inc/phy.h` is **deleted** rather than copied. Verified in both
directions: the device's ROM is byte-for-byte the same size across the move, and
**removing the shared header fails the device's build** at `phy_sx126x.c:9` and
`hublogic.c:11`, which is ADR-0028 phase 9b's exit criterion demonstrated on one
header. It lived in `Common/inc/` and reached the device through `OPENHUB_PATH`
until phase 9 step 6; it is `radio_stack/inc/phy.h` now and both firmwares reach
it through the submodule, which is the same claim over a different path.

That is the cheap half. **What is left is the hub having no implementation of it**
— `radio.c` still calls the driver directly, and the survey below is what has to
move behind the contract:

| Kind | Distinct | Sites | Where |
|---|---|---|---|
| configuration — bitrate, deviation, sync, packet format, power, DIO, DAGC, AFC, LNA, bandwidth, addresses, FIFO threshold, osc calibration | ~20 | ~20 | all inside `RFM_Init`, each called **once** |
| operation — `set_mode_blocking` 10, `read_reg` 9, `set_carrier_hz` 7, `read_fifo` 7, `set_mode` 6, `measure_rssi` 3, `get_rssi` 3, `write_fifo` 2, `get_irq_flags` 2, `wait_irq2`, `get_afc_raw`, `get_lna_gain` | 12 | ~53 | scattered through the superframe loop |

**Step 2 is done: the configuration is behind `phy_init()`.**
`CM4/Core/Src/phy_rfm69.c` holds the twenty-one configuration calls, the five
platform-glue functions and the `rfm69_dev_t`; `radio.c` lost 81 lines and calls
`phy_init()`, `phy_tune()` and `phy_standby()` instead. `phy_rfm69.h` is
scaffolding and **every declaration in it is a debt** — the driver handle and the
`RegDioMapping1` readback, both of which go when the operation sites move.

Checked three ways, because a configuration move that drops a register builds
perfectly:

- **the call sequence, mechanically**: 23 driver calls before, 23 after, the same
  multiset with the same arguments, extracted from both revisions rather than read
- **the part itself**: `device synctime` prints `RegDioMapping1 02 read back,
  DIO3 asked 2`, so the mapping reached the chip
- **the air and the link**: 16 beacons over ~15 superframes, C0's permutation
  intact, C5b 7 of 7, node A delivering with `frames_bad 0`, and
  `arrival_us 77950` against 77 866–78 063 before the cut — the same timestamp,
  which no reordered bitrate or deviation would survive

**Two calls changed position on purpose** and the reasoning is on
`radio_devices_docs/radio/phy-seam.md`: the carrier, because `phy.h` says the
caller names the channel, and the node address, because it is the hub's own value
and not a PHY constant. The RC oscillator calibration consequently runs *before*
the carrier is set rather than after; that was an argument until the run above,
and is now a measurement.

**Step 3 is done: five of the driver's nine operation kinds are behind the contract.**
`phy_init`, `phy_tune`, `phy_standby`, `phy_listen`, `phy_transmit` and
`phy_rssi_now` are implemented; `radio.c` is **2530 lines** and its direct driver
calls are down from **53 to 30**. `transmit()`, `build_frame()` and `tx_buffer`
are gone from `radio.c`; what stayed is `frame_send()`, which keeps the hub's
`lead_*` statistics because those are the hub's and not the PHY's.

The transmit path was the only one restructured, so it was compared call by call:
**six driver calls before, the same six in the same order after**, the same frame
construction and the same FIFO length. Verified on the bench against four
independent readings, none of them the compiler:

| | before the cut | after |
|---|---|---|
| `tx command to first bit` | min 337, max 504 µs / 727 frames | **min 372, max 501 / 198** |
| node A's arrival stamp | 77 866 – 78 063 µs | **77 927 µs** |
| the hub's own per-device row | delivering | **12 ok / 0 bad, `missed_run` 0** |
| the air | 31 beacons / 31 superframes | **30 / 30**, C0 intact, C5b 14 of 15 |
| downlinks the hub sent | — | **82 of 82 opportunities** |

**Six of nine are done. `phy_now_us` landed once the clock question was settled**:
every span this interface reports rides the **backend's own clock** and the caller
scales, which is what `phy_transmit`'s `air_us` already does. On this part that
clock is TIM2's tick and not a calibrated microsecond, so the `_us` in those names
is a wart rather than a promise — left alone, because renaming a field two
firmwares compile has to be worth more than a better name.

**Step 4 is done: `phy_poll` is cut.** Progress on this item used to be reported
as *seven of the nine*, and that denominator could not be checked against
anything: `phy.h` declares **eight** calls, the survey below counts **nine**
driver operation *kinds*, and the table further down lists **twelve** distinct
operation symbols. Three numbers, one word. What replaces them is one grep each,
measured 2026-08-25:

| population | where | count |
|---|---|---|
| distinct driver symbols `radio.c` reached | `9df30a1`, at 2649 lines | **36** |
| the same today | `c552b06` onward | **10**, over 18 sites |
| the contract that replaced them | `radio_stack/inc/phy.h` | **8** |

The middle row is what this item closes on and it is the one to re-run. The
survey's nine and this table's twelve differ because one groups by kind and the
other counts symbols; neither is wrong and neither describes the header.
`radio_devices_docs/radio/phy-seam.md` § where the seam falls.

`phy_poll` being cut means `rx_note_sync`, `rx_discard_frame`, the flag reading inside
`rx_frame_ready`, the DIO3 interrupt and all four FIFO-read paths are under the
seam; the AFC ring, the per-grid regression, the slot attribution and the floor
measurement stayed above it. `radio.c` was **2467 lines** at that step and its
direct driver calls **18**, from 30. **Counted again on 2026-08-25, after the
library left this tree: 2492 lines and the same 18 calls.** The line count is the
weaker of the two figures and moved without the seam moving; the call count is
the one this entry closes on.

`phy_ev_t` gained four fields, not three. `afc_hz`, `afc_valid` and `lna_gain`
were the ones this entry already owed. The fourth is **`sync_seq`**, and it is
there to stop the cut deleting an instrument: this hub counts one sync event two
ways — a polled latch (`sync_match`) and a DIO3 pin interrupt (`edges`) — and
their disagreement is information. A four-kind event reports what a *poll* saw, so
on its own it would have collapsed the pair into one number. `sync_seq` is the
ISR's own counter snapshotted into every event, which recovers both and also
replaces the `sync_edge_new` flag with something that cannot lose two edges
silently. A fifth, **`busy`**, replaced the caller's `!(flags1 & SyncAddressMatch)`
guard on the floor sampler.

**The northbound schema did not move, and this entry said it would.** The wire
field is already `OHT_F_DEVICE_AFC_HZ` and `ipc_afc_t` already carries hertz; only
`ipc_afc_raw_t.afc[]` was in Fstep, converted by an `IPC_AFC_STEPS_TO_HZ` macro
that hardcoded **FXOSC = 32 MHz** inside a header that is supposed to know no
chip. The unit moved below the seam and that macro is deleted. `test_oht` reports
the same schema hash across the change.

**Two behaviours changed on purpose.** The join path now recovers a dirty FIFO the
way the uplink path always did — it used to return without flushing — and
`phy_poll` restarts the receiver on any refused poll, so `flushes` counts a
slightly wider set.

Verified on the board against five readings taken on both sides, because the
superframe-base reconstruction is the part that could have broken quietly: the
arrival offset (70747 → 70705 µs, min 70656 → 70705, **0 implausible** both
sides), AFC reads failed (0 both sides) and its range, the per-frame level, slot
and gain (slot 2, G1, -40…-44 dBm both sides), the ladder identity
`sync == frames + crc` holding at 3 = 2 + 1, and node A still delivering with
`0 bad`. The uplink floor sampler, which the `busy` guard governs, reads
-84 / -95 dBm.

**Step 5 is done, and it is the first return on the seam: the receive path has a
host test.** `CM4/test/test_phy.c`, 95 checks over a fake part, building
`phy_rfm69.c` for the host across two shims and wrapping the driver's own
`fake_spi.h` rather than copying it. Every case is a rule this project learned on
the air and none of them is visible to a compiler: an edge counted as a level, a
trigger destroying the latch it was meant to read, the carrier error read after
the drain has re-armed the receiver, a corrupt length byte believed.

**It ships three mutation controls and it has been pointed at the code.** Three
defects were written into `phy_rfm69.c` on purpose and each was caught by the case
that owns it — the latch counted as a pulse, the CRC frame left in the FIFO, and
the level taken with a trigger. The suite also refuses a run whose check count has
shrunk, because a deleted case is otherwise indistinguishable from a passing one.

What it does **not** cover is the wiring: the shims come first on the include
path, so a change to the real `main.h` does not reach it. That trade is stated in
its `Makefile` and it is the right one — the suite is about what `phy_poll` does
with a part, and the board is what the bench is for.

What is left is eighteen calls and they are a coherent residue rather than
leftovers: `read_reg` 7 and the four SPI-loopback calls are the **debug surface**,
`measure_rssi` 3 are the floor samplers that want a half-decibel `phy_rssi_now`
before they can move, and `set_rx_bandwidth_hz`, `rx_bandwidth_from_reg`,
`set_node_address` and `set_lna_gain` are console setters.
**`phy_poll` is the one that gives `PHY_EV_CRC` somewhere to go**,
and it is where the seam stops being tidying and starts being an instrument.

**There is a second residue and this entry has been counting only the first.**
Beside those eighteen driver calls, `radio.c` holds **67 timebase call sites**,
and `phy_now_us()` appears in it exactly **once**:

| call | sites |
|---|---|
| `timebase_us_to_ticks` | 21 |
| `rfm_micros` | 18 |
| `timebase_elapsed` | 15 |
| `timebase_ticks_to_us` | 13 |

**None of the 67 names a chip, which is why the grep that found the first residue
could not see this one.** `rfm_micros` is not a driver call at all — it is
`CM4/Core/Inc/timebase.h` over TIM2, hub platform glue wearing the driver's
prefix — so the file is a fifth of the way off the RFM69 and no distance at all
off this board.

That matters because it is the exit criterion, not tidiness. Phase 9a is graded
on *the superframe arithmetic, window placement and the exchange FSM run as host
tests with no board*, and moving eighteen `rfm69_*` calls does not reach it while
67 sites hold the grid on TIM2. **Thirty-four of the 67 are the two conversions
that scale by the LSE calibration window**, and the device's `timebase.h` has no
equivalent of either — so this is also what phase 9b runs into the moment one set
of sources has to satisfy both trees.

The seam under it is four operations and the fix is small:
[ADR-0029](../radio_devices_docs/radio/decisions/0029-the-library-declares-four-backends-and-absorbs-no-control.md)
declares `timebase.h` as a fourth backend, this firmware supplies
`timebase_now()` as `return rfm_micros();`, and the 18 sites convert as part of
the move rather than as a rename — `rfm_micros` keeps its misleading name in the
hub's own 30 sites, which buys nothing to change.

`radio_devices_docs/radio/phy-seam.md` § the clock is the backend's.


2649 lines, **82 references to `rfm69_` and 62 to the mailbox**, interleaved with
the superframe grid, the roster and the pairing state machine. The cost is not
tidiness: **no claim about the logic can be checked without the chip**, and the
chip is the hardest part of this system to observe.

The seam is measured rather than guessed and it is narrow. Of 29 distinct driver
calls, **20 are configuration and every one runs exactly once** from values that
already live in `radio_phy.h`; nine are operations. That nine is `phy.h`, which
the device tree already carries with `phy_sx126x.c` under it.

**This is a K2 instrument before it is a refactor**, which is the whole argument
for its position. It buys two things no counter on this side can: the grid becomes
host-testable against a fake PHY that returns frames on command, and identical
logic on a second chip separates *the logic is wrong* from *this driver is wrong*.

`PHY_EV_CRC` is the small case that shows the shape. Today a frame that fails CRC
increments a counter and dies inside `rx_frame_ready`, while `sync_rssi_sample()`
measured its level and threw it away — so **a corrupt frame is a silence with a
counter instead of a frame with a level**, which is exactly the number the join
region investigation needed and could not get.

Ordering, from [ADR-0028](../radio_devices_docs/radio/decisions/0028-the-radio-is-a-library-and-the-region-is-a-compile-time-profile.md):
cut the PHY seam here, then get both firmwares onto one set of sources **in
place**, and only then split the repository. The mailbox is the second and larger
seam and is not this item's.

`../radio_devices_docs/radio/phy-seam.md`.

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
- A resync has never been run on either node — read `ident` off the board for
  the current id, which changes whenever a store is erased.
- A constant for radiated energy in the duty-cycle model — **model only, never a
  roll-back**, so it must not be wired to anything that suppresses a transmit.
- A bracketed `timing` / `syncstats` / `timing` read over a named superframe
  window, so their `no-beacon` count has `beacon_n` as a denominator.
- **Warning before the next hub reset**, so node B is parked and instrumented
  when it happens. Park-and-wait recovery is built on their side; an unannounced
  reset spends the one honest test of it on nobody watching.

`open_hub/testing/sdr.md`.

### 76. The beacon's air time is 48 % over what the payload predicts — `defect`

Regression run `2026-08-24-1`, check RG-A-5. Over 31 superframes `airgrid` puts
the beacon's mean air time at **5.90 ms against a 4.00 ms prediction**, computed
from `phy.air_us()` off the compiled headers. `radio_devices_docs/specs/06-regression.md`
§6.1 sets the tolerance at ±10 % relative; this is +48 %.

**It is not yet attributed**, and the two candidates need separating before
anything is changed: the hub genuinely keying the carrier longer than the frame
needs, or `airgrid`'s burst detector bridging the beacon with an adjacent edge.
The downlink in the same window measures 8.50 ms against 8.00 ms predicted — 6 %,
inside tolerance — which argues the detector is not generally long, and therefore
points at the beacon itself. That is an argument, not a measurement.

It matters beyond tidiness: the beacon is the one frame every device must hear
every superframe, and its length is a term in the hub's own duty cycle — measured
at **0.295 % for beacons alone** in that window, against the 0.200 % the idle-hub
prediction gives.

The instrument to settle it is `spectrum.py` on a single plucked beacon, which
needs no new air.

### 77. `bandscan.py` is new, is load-bearing, and has no home in the test plan — `debt`

Written during run `2026-08-24-1` under the `regression` skill's rule for analysis
tools. It reports per-channel occupancy across a wideband capture and it exists
because `airgrid.py` returns everything off-grid as one undifferentiated list, in
which a foreign carrier and a missed uplink are indistinguishable — and they have
opposite consequences, one a defect and the other a reason a run is void.

It carries `--self-test` with both arms and refuses on an empty population. What
it does **not** have is an `RG-` id, so nothing runs it on a schedule.

Two of the run's three instrument defects were found by it and are worth keeping
against whoever edits it next:

- **The zero-IF DC spike lands on a grid channel** and reads as 100 % occupancy at
  the loudest peak in the capture. Notched, and the notched channel is *named* —
  a channel the tool cannot see is not a channel that is quiet.
- **An analog filter no narrower than the sample rate puts its transition band
  inside the analysed span**, and the rolloff reads as 12–20 % occupancy on four
  grid channels. Re-centring the receiver moved the "traffic" to the new band
  edges while the accused channels fell to 0.2 %. `capture.py` still **defaults
  `--bandwidth` to the sample rate**, so every capture taken without passing it
  explicitly carries this, and that default is the real defect.


### 78. The server's `connected` is a blind instrument for up to two minutes — `defect`

`/api/hub` reports `connected` from the socket's own state, and nothing times out
a hub that stopped answering. Measured on 2026-08-24 across three resets: the
server noticed the dead socket after **10 s, 80 s and 121 s**, and in two of the
three the hub had already redialled before the drop was registered at all - so the
transition never appeared.

Both readings are wrong in a way that matters. A reader polling `connected` sees
**up straight through a reset it never observed**, and later sees **down long after
the hub is back**. A window partitioned on it is partitioned on a boundary that
does not correspond to anything the hub did.

`boot_id` already carries the truth and changes exactly once per reset. The fix is
either a keepalive deadline that marks the link down on silence, or - cheaper and
strictly better - publishing the reset as a **`boot_id` transition** so a reader
never has to infer one from a connection state.

This one cost time on 2026-08-24: a hub whose radio was healthy read as a stale
device record for six minutes, and the first hypotheses were about the radio.

### 79. The northbound link took seven minutes to return, once — `defect` `not reproduced`

**Seen once, on 2026-08-24, after a CM4 reflash.** The hub ran normally on its own
console throughout — CM4 accepting frames, the grid advancing, `devices` healthy —
while `telem` reported `down, last reason: connect failed (accepted)` with
`connect failures 2`. It redialled on its own after about seven minutes.

**Three arms failed to reproduce it, and they are recorded so nobody spends them
again:**

| arm | link back after |
|---|---|
| software reset (`--rst`) | 4 s |
| hardware reset (`-hardRst`) | 0 s |
| flash + verify + software reset — the exact operation that produced it | 0 s |

**The hypothesis that was tested and not supported**: on a NUCLEO-144 the Ethernet
PHY's reset is tied to the MCU's NRST, and `--rst` is a *software* reset that never
pulses the pin — so the PHY would come up unreset after every reflash. The premise
is confirmed (`STM32_Programmer_CLI` prints `Reset mode : Software reset`, and
`-hardRst` is the one that drives the pin) and the consequence did not appear.
Something else caused the seven minutes.

**What would make the next sighting diagnosable**, since none of it was captured
this time: `telem` and `ip` read *during* the outage rather than after, `lwip`
statistics, and whether the host saw any SYN at all. Item 78 is a prerequisite for
even noticing the outage promptly — the server's `connected` flag was up to two
minutes stale throughout, which is why this was found late.

### 82. The server's roster UI has never met a real hub — `debt`

`openhub-server` enrols and removes devices from its page as of
`feat(ui): enrol a device and remove one, from the page`. Every path was driven
against `fakehub` and in a browser; **none of it has been run against the H755.**
What the fake hub cannot answer, in the order to answer it:

- **`device_add` for a device the store does not hold.** `hub.pair_state` reaches
  `listen`, the page's countdown and `RADIO_PAIR_WINDOW_MS` agree to the second,
  `paired_total` moves by exactly one on the join, and the device appears in the
  table on its first report rather than at the join.
- **Whether `quiesce` is observable at all.** It lasts
  `RADIO_PAIR_QUIESCE_SUPERFRAMES` = 2 superframes, about 4 s, and a snapshot is
  `OPENHUB_SNAPSHOT_MS` = 5 s apart, so it may pass between two snapshots every
  time. If it does, the page's one diagnostic separating *the device never
  answered* from *the exchange started and died* is decorative, and the fix is a
  pushed event rather than a faster poll.
- **`device_add` for a device already paired** — the re-pair. `cfg_enrol()` keeps
  the slot, bumps `key_gen` and wipes both keys; confirm the node rejoins rather
  than going silent, since this is what an operator gets when they retry.
- **`device_remove`, both halves.** `ok` against `radio_err`, and whether the node
  keeps reporting after its own removal — the page's `reported_since` marks the
  radio's half and no real hub has ever produced one.
- **Pre-register the attempt count before any of it.** Item 59 puts a fresh
  enrolment at about 1 in 5 once anything is paired, so a single `no_join` says
  nothing about this page, and reading one as a defect in it is item 59 missed.

**The copy this would stop needing** — `contract`, so it is agreed before it
lands: `OPENHUB_PAIR_WINDOW_MS` duplicates `RADIO_PAIR_WINDOW_MS` because no
telemetry field carries the window, and a copy of a compile-time constant is
wrong the first time the constant moves. `ipc_pair_state_t` already holds
`window_left_ms`, `dev_id`, `reqs_seen` and `reqs_dropped`, and `telemetry.c`
already reads the struct — publishing those four is a schema addition plus two
lines there, and it turns the page's countdown from an expectation into a
measurement. `EVT_PAIRED` and `EVT_PAIR_WINDOW` are wire codes nothing sends,
which is the same gap seen from the other side.

`../openhub-server/README.md` § The roster.
