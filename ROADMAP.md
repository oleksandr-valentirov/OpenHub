# Roadmap

The single list of open work: debts, defects, and design that was agreed and
never built. **Nothing here is reasoning** — every item names the page in
`../radio_devices_docs` that holds the why, exactly as source comments do. If an
item needs a paragraph to justify it, that paragraph belongs on its page and the
line here shrinks to a pointer.

**Cite a tag or a commit message, never a bare SHA.** A rewrite orphans a hash
silently and neither the writer nor the reader is told.

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

Five of the nine clauses are done and verified on air and four are partial. What
is left is below.

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

### 29. A device that keeps its counter cannot transmit for up to 33 minutes — `blocking` `device` `contract`

The mirror of item 5 and worse: that one is a device that **lost** its counter
and cannot find the hub; this is one that **kept** it and may not speak. Found
2026-08-21 by the device session on its own board — `STORE_COUNTER_STEP` 1000,
the reserved mark pushed to ~644552 while the hub was at 644181, every genuine
beacon refused as a replay 368 superframes "in the past".

Two faults sit in it. **The durable mark gates the wrong thing** — it answers
*which counters may I seal with* and was also answering *what time is it*; the
device session's fix splits beacon replay from nonce safety. **Splitting them
does not remove the outage**: nothing may be sealed below the reserved mark, so
at a step of 1000 that is **up to 33 minutes of silence after any reset**, by
design, announced nowhere.

**The hub's half is to stop reading this as a fault.** `devices` shows `never`
for a device that has not reported, which covers a device that is gone, one that
is deaf and one that is serving its reservation with one word — and the hub is
the only place an operator looks.

`radio/known-issues.md`.

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

---

## Defects

### 7. A join beacon shares every invitation's superframe while nothing is paired — `defect`

`PAIR_INIT` targets round up to `RADIO_PAIR_INIT_EVERY` (4) and the join beacon
runs every 2, so **every** invitation lands on a beacon superframe. The
`device_count != 0` branch of `join_region_service()` suppresses the beacon
there; the `device_count == 0` branch — a hub that has never paired anything,
which is the first-pairing case — does not. Both are keyed at `join_offset_tk`
about 8 ms apart.

By name the fault ADR-0021 records: the device heard 15 beacons and no
invitations until the beacon was suppressed. **Found by reading, not on air**, so
what it costs a receiver is unmeasured.

`CM4/Core/Src/radio.c:1700`.

### 8. The LSE measurement is unexplained at the window level — `defect`

The mean is sound — it matched a host-clock measurement to 51 ppm over ten
minutes — but a single 7.8 ms window carries ~350 ppm of noise that a sixteenfold
longer window barely reduced. That should be impossible: the accumulated span
telescopes to two timestamps. Averaged over ~32 s it does not block the grid.
`RCC_BDCR.LSEDRV` is at its lowest reset default and is the untested lever.

`open_hub/radio/timebase.md`.

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

- **`PAIR_REQ` still carries `pubkey[33]` and is still 57 bytes**, not 24. The
  size is asserted, charged and pinned in three places, all agreeing on 57.
  Buys 10.6 ms of device air for a coordinated wire change.
- **The broadcast join beacon is still transmitted.** The decision removes it;
  the implementation only suppresses it on invitation superframes — and item 7
  is that even this is incomplete.

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

### 23. The device's channel filter is 5 kHz too narrow — `contract`

`radio_phy.h` carried one `RADIO_RX_BANDWIDTH_HZ` of 125000 with a comment saying
the device used 117300, and the Carson assert compiled the hub's number on both
builds — the same shape as an assert tying two definitions one side owns. With
the carrier error measured, the arithmetic is no longer hypothetical:

    required at 11230 Hz measured    122460
    hub  125000                      passes, 2540 Hz of margin
    device 117300                    short by 5160 Hz

The constants are now named apart, `RADIO_RX_BW_HUB_HZ` and `RADIO_RX_BW_DEV_HZ`,
against a shared `RADIO_RX_BW_MIN_HZ`. **Only the hub's is asserted**, because
asserting the device's fails both builds today and the configuration change is
the device's to make.

**Measured before it was configured, and the measurement says wait.** Over 19
cycles the device lost **0 of 19 beacons** on the short filter, and 125000 is not
available to it — the SX126x table steps 117300 to 156200. The budget also
predicts the wrong side: the receiver short of it loses nothing while the
receiver that clears it by 342 Hz fails nearly every frame. What survives is the
header defect; the second assert waits for a measurement that asks for it.

`radio_devices_docs/radio/phy.md`.

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

### 54. A linker script edit does not relink — `defect`

`touch CM7/custom_m7_flash.ld && cmake --build CM7/build` prints **`ninja: no work
to do`**, and the same on CM4. The script reaches the link only as the `-T`
argument inside `rules.ninja`; nothing lists it as a dependency edge.

The failure mode is silence: editing the memory map, moving a section or changing
an origin rebuilds cleanly and produces the binary that predates the edit. Until
it is fixed, **delete the ELF before rebuilding after any linker script change.**

`open_hub/arch/build-and-generation.md`, `open_hub/arch/memory-map.md`.

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

### 56. `device remove` forgets a device on one core only — `defect`

`cmd_device_remove()` calls `ks_forget()` and sends CM4 nothing, under a comment
saying CM4 "holds no per-device state yet". That stopped being true: CM4 keeps
`devices[RADIO_MAX_DEVICES]` with each device's keys, slot and downlink state,
installed over `IPC_REQ_INSTALL_DEVICE`.

`used` is written in exactly one place — set to 1 at install — and **cleared
nowhere**, so a removed device keeps its slot on the radio until the next reset:
its uplinks are still accepted, still decrypted and still reported northbound,
and downlinks still go out to it. The operator is told `removed 0x...`.

`IPC_REQ_REMOVE_DEVICE` exists in the shared enum and CM4's handler answers it
`IPC_ST_UNKNOWN_REQ` by falling into `default`, so the request is reserved and
refused rather than missing. The fix is to implement it and to send it, and the
control is the one the CLI already hints at: remove, then re-enrol, and check
that CM4 holds one entry rather than two.

Found by a cleanup sweep for requests only one core knows about — not on air.

`open_hub/arch/ipc.md`, `open_hub/cli.md`.

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
`ip static`. Both wait on a configuration store that does not exist; the `cfg`
command that stubbed it out was removed on 2026-08-22, so nothing on the console
advertises it any more. Until the store is built the link cannot come up
unattended, which is most of what a server is for.

`open_hub/network/telemetry.md`, `open_hub/network/ethernet.md`.

### 39. A device command still cannot say anything the wire has no word for — `debt` `contract`

`dev_app` reaches the firmware end to end and is refused there with
`not_implemented`: the downlink's `app[6]` is unwritten, which is *item 3* seen
from the north side. Everything above the radio is built, so agreeing the payload
with the WL55 session is the only work left and it lands as one commit in
`radio.c`.

`open_hub/network/telemetry.md`, `radio/tdma.md` § slot budget.

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
Still needs a decision record. Next free number is ADR-0026.

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

Arm B is six trials and not the ten pre-registered, because item 66 stopped the
store mid-batch. Its four remaining trials are void and are not counted.

**What the arms did not kill is the carrier.** `device afc` over each arm:

| arm | frames | min | max | last |
|---|---|---|---|---|
| A | 48 | -12 330 | **19 287** | 13 061 |
| B | 15 | -10 132 | **17 089** | 8 666 |

`RADIO_CARRIER_ERR_HZ` is **12 000** and both arms exceed it — in the frames that
*arrived*. The lost ones are not in that sample, so it bounds nothing on its own,
but the allowance is demonstrably being spent. The untried arm is
`RADIO_RX_BW_HUB_HZ`, whose next encodable step is 166.7 kHz for 0.97 dB of noise
bandwidth against roughly 50 dB of margin.

`../radio_devices_docs/radio/pairing.md` § the request that reached the antenna.

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

### 65. `status 1` means two different faults — `debt`

`hub_ipc_call` returns **1** both when CM4 replies `IPC_ST_UNKNOWN_REQ` and when
CM7 cannot take the IPC mutex inside `IPC_REPLY_TIMEOUT_MS`. The CLI prints
"Error: CM4 rejected it, status 1" for both, so a mutex CM7 never acquired is
reported as a request CM4 refused.

Seen on 2026-08-23: `device pair` printed it while `devices`, `timing` and
`device dump` all answered normally, and the same command succeeded on the next
attempt. Half an hour went to looking for an enum mismatch that was not there.

A negative return for the local failure would separate them; `-1` is already
"CM4 did not answer".

`../radio_devices_docs/open_hub/arch/ipc.md`.

### 66. The device store stops accepting writes and cannot say why — `blocking` `defect`

`device add` began returning `not enrolled: flash write failed` partway through a
batch on 2026-08-23 and has not accepted a write since, across a reset and a
reflash of both cores. `device list` reads:

```
1 enrolled, flash: 21 writes, 5 errors, 1736 slots left
last flash error 0x00000000
132 slot(s) hold records of an older format and are skipped
```

**It is not exhaustion.** 1736 of 2048 slots are free. Five writes of twenty-one
failed, so it is intermittent rather than a wall, and the failures survive a
power cycle because the state is in flash.

**`last flash error 0x00000000` is the second defect and the one that blocks
diagnosis.** The store reports an error and then reports its code as zero, so
"which HAL error" is unanswerable from the console. A code that reads zero for a
failure is indistinguishable from no failure ever recorded, which is the class
this project has already been bitten by.

The 132 old-format slots are expected — the store steps over them by design and
they cost a slot each — but they have never been seen beside a write failure and
they are the only unusual thing in the picture.

**Recovery is not free and needs a decision.** This store is in bank 1, sectors 6
and 7, and never erases anything: `keystore.md` is explicit that a full store is
not recoverable by reboot, unlike CM4's. The only documented repair is
`STM32_Programmer_CLI -c port=SWD sn=<probe> mode=UR -e 6 7`.

**That erases the hub's own long-term X25519 key with it.** `ks_hub_key_get` and
`ks_hub_key_set` keep it in this same log, so the repair gives the hub a **new
identity**: every device that ever paired with it must re-enrol, and the public
key the invitations carry changes. On a bench with nothing paired that is cheap,
and it is not cheap anywhere else.

Enrolment cannot be measured at all until this is cleared, so it blocks 63 and 60.

`../radio_devices_docs/open_hub/arch/keystore.md`.
