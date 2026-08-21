# Roadmap

The single list of open work: debts, defects, and design that was agreed and
never built. **Nothing here is reasoning** — every item names the page in
`../radio_devices_docs` that holds the why, exactly as source comments do. If an
item needs a paragraph to justify it, that paragraph belongs on its page and the
line here shrinks to a pointer.

**Cite a tag or a commit message, never a bare SHA.** A rewrite orphans a hash
silently and neither the writer nor the reader is told; `e2e5ed0` sat in item 1
for a day pointing into history reachable only from `pre-squash-2026-08-21`.

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

**"No side commands from a CLI" means none typed on the device.** The operator's
`device add <id> <pubkey>` on the hub is the out-of-band enrolment step and the
criteria do not forbid it — settled 2026-08-21, recorded because the other
reading would have made the threat model itself look like an open item.

**Slots are never chosen by an operator — settled 2026-08-21.** `device add <id>
<pubkey>` is the whole of the out-of-band step; the hub assigns the slot and the
two ends negotiate everything else between them. This retired a proposed
`device add … [slot]` argument that the device session wanted for a guard
measurement, and it constrains the event schedule: the base slot is granted at
enrolment and the other opportunities are **derived by contract constants**, so
no operator picks any of the three.

**Irregular messages are out and a deadline is in — settled 2026-08-21.** There
are no contention windows and no transmit outside the grid; instead the whole
protocol owes a sensor event delivery *and* hub-side processing within 1 s. The
clause that had no code at all is gone, and the deadline it is replaced by is
not met by the grid as built.

Five of the nine clauses are done and verified on air and four are partial. What
is left is below.

---

## Blocking

### 1. Three opportunities a superframe, built and not yet on air — `blocking` `contract`

A device got one slot per superframe, so an event waited up to **2 s — twice the
deadline** — and the contention windows that were the plan are forbidden by the
same decision that set the deadline. The deadline is bought only with the largest
gap between a device's transmit opportunities, and **three per superframe are the
minimum**: two bottom out at exactly 1000 ms, which fails for any positive air
time.

Landed as the pair `054c17f` + `95d6625`, which the shared header's air-time
assert shows is **one change**: 50 kbps, deviation left at 25 kHz, 194 slots of
9400 µs, stride 65, **exactly 64 devices**, the 1400 µs guard untouched, largest
gap 778 ms and 210 750 µs left for the hub.

**Blocked on two things, not one.**

*Item 12* is what the first frames at the new rate found: 21 sent, 10 reached
sync, 10 failed CRC, none accepted. The geometry is not what is wrong.

*The device's replay floor*, confirmed 2026-08-22 and its item 21. The hub's is
the tuple `(superframe, slot)` and the device's is the superframe alone, so the
second and third frames of every superframe evaluate to zero age and are
**refused as replays — two of every three.** It is refused after the tag
verifies, so it presents as a replay counter climbing on a healthy link. Until
that lands, a k = 3 PER run measures the floor and not the radio.

What is left is the part no assert can do: **neither firmware has transmitted a
single frame at 50 kbps that the other one could read.** Sensitivity is ~3 dB worse plus about a dB for the
h = 1 demod penalty, and neither of us has read the datasheet row. A PER run at
the new rate is the acceptance evidence, not the host tests.

`radio/tdma.md` § the event deadline, § what k = 3 breaks.

### 2. The hub half of the event deadline — `measured`

The arrival-driven path exists: `handle_uplink_frame()` pushes `IPC_EVT_UPLINK`,
CM7 handles it, and both sides' counters agree frame for frame. The doorbell has
a handler — `HSEM1_IRQHandler` was a weak alias to `Default_Handler` on both
cores with its NVIC line never enabled — so a wake is arrival-driven, with the
20 ms poll left as the semaphore's timeout so a dead doorbell degrades rather
than wedges.

**Measured 2026-08-21, and it is not the problem.** `device latency` stamps both
terms on CM4's clock, so no two cores' ticks are compared:

    frame end to the event leaving   ~484 us   CM4's poll granularity
    the event leaving to its reply    214 us   the doorbell wake
    hub half                         ~700 us   of RADIO_HUB_HANDLE_SLACK_US,
                                               210 750 us: 0.33 %

The 214 us also closes an older question: the doorbell wake is real, not the
20 ms poll timeout wearing its name. That handler was a weak alias to
`Default_Handler` with its NVIC line never enabled.

**The first reading was 7204 us and the error was in the label.** The sync edge
fires 1280 us into the frame, so the term carried 6720 us of the device's air
time under a heading reading "this core's work" — and the budget it was quoted
against already subtracts the uplink air time, charging the air twice. A label
broader than its measurement is the same defect as a report broader than its
check.

**What the deadline now depends on is item 30, not this.** One second is met if
the frame arrives; two thirds of them do not.

`open_hub/arch/ipc.md`.

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
was one transmission. `windows` counts this side's opportunities; the device's
transmissions are the population that matters.

**The hub must not derive the denominator.** An `expected` column on `devices`
would print an assumption with a column heading. The device has to say what it
sent.

`open_hub/cli.md`, and the `verification` skill.

### 5. A device that loses the counter cannot find the hub — `blocking` `device`

The clause *both survive a reboot and restore the link* was exercised from the
hub side for the first time on 2026-08-21, by flashing this firmware, and **the
link did not come back**. Keys, slots and the store all survived; the device's
superframe counter did not, and the hop channel is a function of it.

`time follow` tunes the channel for `superframe_now() + 1`, which after a gap is
the wrong number, so each attempt is a 1-in-28 draw — it took four, three of them
listening on channels chosen by a counter 2990 superframes stale.

**The fix is device-side and costs the hub nothing**: park on one hop channel and
wait, expected 56 s and 99% within 254 s, then verify the counter heard maps to
the channel parked on. Listed here because ADR-0021 removed the broadcast join
beacon on an analysis that says this case does not exist, and the hub must not
assume an invitation reaches a device nobody is inviting.

`radio/joining.md` § re-acquisition after a counter loss.

### 6. The acceptance run has never happened as one sequence — `blocking`

Every part is verified separately and the whole has never run: clean hub, clean
device, enrol, invite, exchange, encrypted traffic, reboot both, recover.
Pairing has run **one device, once**.

Sweep the success-only paths while doing it. The evening pairing first worked,
five defects surfaced in code that could only execute after something finally
went right — `GET_DEVICE_INFO` read its index from the wrong byte and had failed
every call since it was written, because printing a row needs a paired device.

`open_hub/testing/on-target.md`. Never erase bank 1 from CM7.

### 29. A device that keeps its counter cannot transmit for up to 33 minutes — `blocking` `device` `contract`

The mirror of item 5, and worse. That one is a device that **lost** its counter
and cannot find the hub. This is a device that **kept** it and may not speak.

Found 2026-08-21 by the device session, on its own board, in full:

    STORE_COUNTER_STEP        1000 superframes
    reserve_extend at arming  pushed the durable mark to ~644552
    the hub was at            644181
    every genuine beacon      refused as a replay, 368 superframes "in the past"

Two separable faults sit in it, and the second is the one that blocks.

**The durable mark gates the wrong thing.** It answers *which counters may I
seal with* — a statement about nonce space — and it was also answering *what
time is it*. Aligning a clock to a beacon is timing, not crypto. The device
session's fix is to split the guards: beacon replay against the last accepted
beacon counter, nonce safety against the durable mark. It is security-relevant
and deliberately not being written at three in the morning.

**Splitting them does not remove the outage.** Nothing may be sealed below the
reserved mark, because everything under it may already have been used. At a step
of 1000 that is **up to 33 minutes of silence after any reset**, by design, with
nothing anywhere announcing it.

Against an acceptance criterion that says a sensor event is delivered and
processed **within one second**, and a clause that says both ends survive a
reboot and restore the link. A device that reboots is out of contact for half an
hour and the hub cannot tell that from a dead device.

**The hub's half is to stop reading this as a fault.** `devices` shows `never`
for a device that has not reported, which is correct and useless here — the same
display covers a device that is gone, one that is deaf, and one that is serving
its reservation. Nothing on this side distinguishes them, and the hub is the
only place an operator looks.

`radio/known-issues.md`, and the device session's item on the counter store.

### 30. Two thirds of the device's frames never arrive, at a level called healthy — `blocking` `defect`

Found 2026-08-21, against a denominator the hub cannot compute: the device
session counted what it sent.

    rate 8   62 frames sent, 19 delivered    31 %
    rate 2  291 frames sent, 105 delivered   36 %

Both at **-17 dBm**, the level at which item 12's arm reads 15 of 18 accepted
and which both sides have been calling healthy. The 4x cadence change does not
move it, so there is no receiver ceiling and no cadence effect.

**A third window, 2026-08-21 23:2x, is worse than either and is the one to
believe**: sf 647403..647694, the device transmitting 151 cycles on all three
opportunities, 453 frames. The hub took 103 sync edges and accepted 90 — **23 %
detected, 20 % accepted.** It is lower than the rows above because those two used
a denominator counted over a differently-bounded window; this one was counted by
both sides over one agreed superframe range.

**It hid inside a numerical coincidence.** 100 % of rate-8 traffic, 31 % of
rate-8 traffic and 27 % of rate-2 traffic are all near 0.4 frames a superframe.
Every reading taken that evening landed in that band for three different
reasons.

**And it hid behind the wrong fraction.** `accepted / delivered` is 81 %;
`delivered / transmitted` is 31 %. The first was quoted and a conclusion drawn
about the second. Only the numerator exists on this side, which is item 4's
point arriving from the other direction.

Losses are not flat across the three opportunities:

    slot   1    55 of 97   57 %
    slot  66    30 of 97   31 %
    slot 131    20 of 97   21 %

and the agreed window says the same shape four times lower — accepted over what
the device transmitted, 151 per slot:

    slot   1    59 of 151   39 %      (64 detected, 42 %)
    slot  66    22 of 151   15 %      (24 detected, 16 %)
    slot 131     9 of 151    6 %      (15 detected, 10 %)

**The first reading of this window put the per-slot counts over 89 and got
72/27/17 %.** 89 was this side's own *accepted* total, so each numerator was
divided by a sum that contains it. It survived a read because the device's
progress report an hour earlier had also said 89 — a partial count of a different
quantity that happened to collide — so the two halves looked like one population.
The device session caught it. `verification` skill § numerators and denominators.

**`ok` and `CRC FAIL` have to be carried separately per slot from here on.** The
late opportunities do not merely go missing; the frames that do arrive fail CRC
more often — 6 of 15 at slot 131 against 5 of 64 at slot 1. Small numbers, no
conclusion, but "never detected" and "detected and broken" are different faults
and only this side can tell them apart: every frame leaves the device identically
and its instrument reads both as a transmit with no consequence.

Monotonic with offset into the superframe, which is the shape of anything that
scales with that offset — including a clock-scaling residual on either side.
**Neither side's placement instrument can see that class**: each measures its own
frames against its own computed target, and an error in the target leaves both
readings flat.

The discriminator is agreed and needs no flash: a single-slot window. If slot 131
alone delivers ~21 % the loss tracks the offset; if it delivers ~57 % the loss is
competition or recovery inside the hub's receive window.

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
what it actually costs a receiver is unmeasured.

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

**k = 3 promotes this from tidiness.** A device that used all three opportunities
every superframe sits at **1.200%** at 50 kbps and **2.400%** at 25 kbps, so the
deadline caps the sustained event rate as well as costing capacity — and nothing
in either firmware would refuse, every individual frame being legal. The device's
bound is a budget over the regulator's hour (36 s of air), not an integer per
superframe: the hub's `SUPERFRAME_US / 100` rule is a stricter proxy that suits
scheduled traffic and not bursty traffic.

The SDR cannot referee it either. `--expect-ms` selects our bursts by air time,
and halving the frame while tripling the count is what invalidates that
selection, so the instrument needs re-validating at whatever rate is chosen
before it can gate anything.

**Both figures above were the 42-byte wire until 2026-08-21** — 1.008% and
2.016%, carried in prose on both sides and never recomputed when `link_v4` took
the uplink to 50 bytes. Found by the device session. `RADIO_DUTY_PPM` now
derives them from `RADIO_UPLINK_AIR_US`, so the next wire change moves the
number instead of leaving it behind: **a budget held in frames is a budget a
wire change edits silently.**

`radio/phy.md` § duty cycle, § the device's budget.

---

### 11. A zero timebase scale would wedge the grid, and only luck forbids it — `debt`

`superframe_due()` in `CM4/Core/Src/radio.c` steps the boundary by
`superframe_tk` in a `while` loop, so a zero step is an unterminated loop with a
runaway counter, and `timebase_ticks_to_us` divides by the same scale. The device
session hit exactly this on 2026-08-21 — a clock aligned with a stub period of
zero, counter at 117 948 578, console dead with the core.

It cannot fire here, and **the reason is two derivations away from the value**:
`calib.c` rejects any capture window outside ±2 % of nominal, so the average
those windows feed can only land near `1 << 24`. Nothing between
`timebase_set_scale`, which takes any `uint32_t`, and the loop that trusts it
would notice a zero.

The fix is the one the device session arrived at: make the bad value
unrepresentable at the setter rather than checked at the use. Deliberately not
folded into the grid commit — a defensive-path change does not belong in a wire
change.

`open_hub/radio/timebase.md`, and the `verification` skill § failure-path sweep.

### 12. The link fails at high input level, and the mechanism is not settled — `blocking` `defect`

Found 2026-08-21 by the device session dropping its transmit power from +14 to
-17 dBm. Matched windows of 36 frames, attributed frame by frame off two
independently computed hop maps:

    A  -17   57 frames   8 syncs   8 accepted   0 CRC failures
    B  +14   36 frames   1 sync    0 accepted   1 CRC failure
    C  -17   36 frames   9 syncs   8 accepted   1 CRC failure

    accepted  B 0/36 vs C 8/36   p = 0.0025
    synced    B 1/36 vs C 9/36   p = 0.0068
    pooled    low 16/17 syncs accepted vs high 2/20   p = 1.8e-07

C reproduced A on fresh channels minutes later, so drift is excluded. **Sync
moves as well as CRC** — the long-held "sync is fine, CRC is dead" described the
high-power era, which was the only era there was.

**Which end is not settled.** The experiment varied transmit power, which is this
receiver's front end in compression *and* that transmitter's PA at its rated
maximum, perfectly confounded. Both predict every observation. The device session
raised it against its own hypothesis.

**The discriminator has now been run once and it did not settle this.** Pinning
the LNA, reconstructed 2026-08-21 from the whole of the run's ring rather than
from a chosen sub-window:

    pinned G6   18 frames   15 accepted   83 %   level -14..-12 dBm
    G1 pooled   50 frames   28 accepted   56 %   level -45..-25 dBm

    pinned vs pooled G1          p = 0.034  (one-sided)
    G1 before vs after the pin   p = 0.517  <- no time trend, so pooling is licensed

Three things about that table, and the third is the reason the entry stays open:

1. G6 is 30 dB *below* G1, and the reported level went **up** 24 dB. The part
   does not compensate RSSI for the LNA setting, so a linear front end reads
   lower, not higher. Compression explains it; so does the transmitter's power
   having changed mid-run.
2. **Neither the pin nor the transmit power is in the log.** Both had to be
   inferred afterwards from a column, so the run cannot be re-read and cannot
   exclude (1)'s second explanation. Any repeat must log the intervention.
3. It was read three times to three different ratios — 7/7, then 15/17 against a
   hand-picked 2/11, then this. `p = 3.6e-04` was quoted from that 2/11 control
   and does not survive a pooled one. **The effect is p = 0.034 on one confounded
   run**, which is a reason to look, not a mechanism.

The discriminator itself: `LnaCurrentGain`, bits 5:3 of `RegLna`,
is read-only and reports the gain the AGC settled on. It is sampled per delivered
frame in `afc_note()`, because the AGC returns to G1 the moment the air is idle
and a console read would catch the idle value. **If it reads G1 on frames
arriving at +14, the AGC is not backing off under a level measured to break the
link**, and the device's PA is exonerated without proving a negative about it.

A gain with no level beside it does not separate the two, and the device session
said so: G1 at -12 dBm is an AGC refusing to back off, G1 at -53 dBm is an AGC
doing its job on a weak signal, and the two conclusions are opposite. So
`device afcraw` now carries the level as well, read off the latch at
`SyncAddressMatch` (item 14). At the device's -17 dBm it reads **-43 dBm**
against a -96 dBm floor, which puts +14 at about **-12 dBm** — a level this
front end can plausibly be loaded by. The three-way reading is:

    G1 at about -12 dBm   the AGC is not backing off; the device's PA is clear
    G1 at about -53 dBm   the level column is stale and nothing else is readable
    G4..G6                the AGC backed off; the device's PA is the suspect

**The window itself is the control for the column**: 31 dB of transmit power
must move the printed level by about 31 dB, or the column is not measuring the
frame and no gain reading may be quoted from the same output.

The second, independent arm — spectral regrowth at high power — is **void as
captured**. Both wideband files clip the dongle's ADC (0.11 % of samples at the
rails at -17 dBm, 0.17 % at +14, `-g 30`), so any regrowth measured off them is
the receiver's own clipping offered as the transmitter's. Recapture at much
lower gain before this arm is read at all. What the captures do support, because
it is a ratio inside one file, is that the device's three opportunities land at
+59.4, +670.4 and +1281.4 ms after the boundary exactly as the grid says.

The carrier error is measured and **excluded**: corrections span -7691 to +17944
Hz, change sign frame to frame, disagree with themselves by kilohertz on one
channel, and the frame carrying the largest correction ever recorded was
accepted. `RADIO_CARRIER_ERR_HZ`, the 342 Hz of filter margin and the whole
bandwidth class retire with it. Why the reference wanders is open and no longer
blocking.

The hypotheses refuted along the way — bandwidth, deviation, DAGC low beta, a
band correlation, a channel-dependent slope, a separation between the corrections
of accepted and refused frames — are in the `rfm69` and `verification` skills,
which is where reasoning belongs.

`radio/phy.md`, the `rfm69` skill § the carrier is still moving.

### 13. `rx_crc_err` prints under a heading that scopes it wrongly — `defect`

The counter is global and is incremented from the uplink path and the join path
alike. It prints as `rx (this window)` inside `device pair`, so it reads as a
pairing-window statistic and is nothing of the kind. `devices` prints every
uplink refusal bucket and not this one, so nine corrupt frames appeared as a
difference with nothing named as its cause — and were reported to the device
session as being in no bucket at all.

Fourth instance of the class in one day across both benches. Fix the heading, and
print it where the uplink counters are.

`open_hub/cli.md`, and the `verification` skill § a counter's heading.

### 14. `rssi_up` is read from an untriggered latch, and nobody knows what it holds — `defect`

`handle_uplink_frame()` takes the level with `rfm69_get_rssi()`, which reads
`RegRssiValue` and triggers nothing; the superloop's `rfm69_measure_rssi()` runs
only in the branch where sync is clear. So the value is whatever the part last
latched, and the code cannot say what that was.

**The first version of this entry said it was therefore stale, and that is not
established.** The receiver's startup sequence gates its AGC and AFC phases
behind RSSI crossing the threshold, so a restart parks until a signal arrives
and the latch may hold this frame's level after all. The measurement argues the
same way: a stale between-frames sample would read near the -92 dBm the part
shows at rest, and `rssi_up` reads -25 dBm, which is what a device a metre away
should produce. A number that survives a plausibility check it had no reason to
pass is not a stale one.

What is actually wrong is that **nothing in the firmware distinguishes the two**,
and the operator is shown a level with no provenance. Sample at
`SyncAddressMatch` instead, which the superloop already notices in
`rx_note_sync()` and which DIO3 is wired for (item 21): the level when the sync
word lands is the only one that says whether a burst was demodulable, and it
needs no assumption about the startup sequence.

**Half done, and the half that is done answers the question the entry asks.**
`sync_rssi_sample()` samples at the sync-match edge and `device afcraw` prints
the result per frame. It reads **-43 dBm against a -96 dBm floor**, 53 dB apart,
so the latch is *not* holding a between-frames sample: the receiver's own gating
measurement writes it at the start of each reception. The first version of the
sampler triggered a fresh measurement instead and failed on every frame — **an
RSSI trigger does not complete while `SyncAddressMatch` is high** — which is why
the counter now counts attempts and failures apart rather than successes alone.

What remains is `rssi_up` itself: `handle_uplink_frame()` still reads the latch
after the frame, and the sealed report still carries a level with no provenance.
`afc_note()` consumes the sync-match sample before the frame handler runs, so
the fix is to stash it for the frame just delivered rather than to read again.

`CM4/Core/Src/radio.c:1378`, `:1625`, `open_hub/radio/configuration.md`.

### 15. The data beacon is unauthenticated — `contract`

So the quiesce flag is a denial-of-service primitive. Bounded by a clamp, a rate
limit, and the fact that a forger must hold the hop key to be on the right
channel — **none of which is a cryptographic guarantee**. The fix is a network
broadcast key, which has the property such keys always have: one compromised node
can forge broadcasts. Worth making deliberately rather than inheriting.

Unlike the join beacon, which is unauthenticated *by necessity* and cannot be
fixed, this one is unauthenticated *because nothing has sealed it yet*.

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
  the implementation only suppresses it on invitation superframes — and item 6
  is that even this is incomplete.

`radio/joining.md` § what ADR-0021 left unbuilt.

### 20. `pair_v3.txt` has no consumer — `contract`

It reproduces `pair_v2`'s published `pair_z1`, and that is all it currently
proves. A vector whose consumer does not exist is untested in the way that
matters — that is how the pairwise-hop-key defect survived cross-verification by
two independent implementations.

`open_hub/security/self-tests.md`.

### 21. `UPLINK_AIM_US` is a contract number that lives on one side — `contract`

Where a frame sits inside its slot is the same kind of number as where the slot
sits inside the superframe, and it belongs in `Common/inc/radio_slots.h`. It is
700 µs on the device — the frame is centred in the slot's slack — and appears
nowhere in the hub, which assumed flush at slot start. Both sides stayed
internally consistent while computing about different geometry. It has already
cost one wrong guard analysis.

**Measured 2026-08-21, and the aim is real.** `handle_uplink_frame()` takes no
timestamp, but the DIO3 `SyncAddressMatch` EXTI instrument does — and it had
been running all along. `device synctime` over 965 edges, slot 1:

    offset in superframe: last 72094 us, min 72031, max 72560

Slot 1 opens at 69 000 µs and the sync edge follows the first bit by 2 560 µs
(preamble 4 B + sync 4 B at 25 kbps), so flush-at-slot-start predicts 71 560 µs
and a 700 µs aim predicts 72 260. **The band's centre is 72 295** — 35 µs from
the aim, 735 µs from the hub's assumption. `UPLINK_AIM_US` belongs in
`Common/inc/radio_slots.h` and the hub's geometry is the side that is wrong.

**That arithmetic is 25 kbps and the run predates the rate change**, which makes
the conclusion sound and the number unusable as it stands. The pre-sync term is
`RADIO_PRE_SYNC_AIR_US`, derived rather than carried, so at 50 kbps it is
**1 280 µs, not 2 560**. The same physical 700 µs aim now predicts a sync edge at
**70 980 µs**, and flush-at-slot-start predicts 70 280.

So the re-measurement has a falsifiable prediction rather than an expectation:
**`device synctime` at 50 kbps must centre near 70 980.** If it centres near
72 295 again, the offset is not being produced by the aim at all and something
downstream of the rate is holding it — which would also bear on the boundary-lag
contradiction in item 31, since both readings subtract the same pre-sync term.

`verification` skill § know which artifact each assert pins.

### 22. `uptime_s` is trusted on arithmetic and unconfirmed on a board — `contract`

**The comment is corrected and the field is believed.** It said the field wraps
at 4294.967 s; it does not. The device session moved the fold out of `timebase.c`
and pinned it across the crossing — 4294 before, **4296 after**, 11496 two hours
past — and the correction is in `Common/inc/radio_protocol.h` as of the `link_v5`
commit.

**Neither side's field data ever reached the test.** The hub's largest
observation is 1496 s and the device's is 4255 s, against a 4295 s threshold —
short by forty seconds. A host test proves the arithmetic, not the board, so the
first device that stays up past 4295 s settles it. Until then this is a claim
about code that both sides read and neither has watched.

What it unblocks, and what is therefore also unbuilt: `devices` prints `never`
for a device that has not reported, covering *gone*, *deaf*, and *serving a nonce
reservation* (item 29) with one word. `uptime_s` splits them — small means
rebooted and says when, large means it never rebooted — and nothing on this side
reads the field that way yet.

`radio/known-issues.md`.

---

### 23. The device's channel filter is 5 kHz too narrow, and the shared assert never saw it — `contract`

`radio_phy.h` carried one `RADIO_RX_BANDWIDTH_HZ` of 125000 with a comment saying
the device used 117300, and the Carson assert compiled the hub's number on both
builds. Green on the device for as long as the rate has been 50 kbps, covering a
filter that side does not use — the same shape as an assert tying two definitions
one side owns.

With the carrier error finally measured, the arithmetic is no longer hypothetical:

    required at 11230 Hz measured    122460
    hub  125000                      passes, 2540 Hz of margin
    device 117300                    short by 5160 Hz

The constants are now named apart, `RADIO_RX_BW_HUB_HZ` and `RADIO_RX_BW_DEV_HZ`,
against a shared `RADIO_RX_BW_MIN_HZ`. **Only the hub's is asserted**, because
asserting the device's today fails both builds and the configuration change is
the device's to make. The next encodable step above the requirement is 125000 —
the hub's own value.

**Measured before it was configured, and the measurement says wait.** Over 19
cycles with the band filter off, the device lost **0 of 19 beacons** on the short
filter. And 125000 is not available to it: the SX126x picks from a table whose
neighbouring steps are 117300 and 156200, so the honest second assert would have
to name a filter 31 kHz wider than the requirement rather than one matched to it.

**The budget also predicts the wrong side.** The receiver short of it loses
nothing; the receiver that clears it by 342 Hz fails nearly every frame. So the
filter width does not look like the term that decides the corruption, and the
hub's own 342 Hz of margin is probably not worth a flash either.

What survives is the header defect itself — one number compiled by two sides, and
the constants are now named apart. The second assert waits for a measurement that
asks for it.

`radio_devices_docs/radio/phy.md`.

### 24. `hub_ipc_call` packs the arg and the payload into one buffer — `defect`

`buf[0] = arg` and the caller's payload from `buf[1]`, so a handler has to know
which shape a request was sent as. It is visible neither at the call site nor in
the handler.

Two handlers got it wrong in opposite directions in one evening.
`IPC_REQ_SET_DEVICE_PARAM` passed a struct and read from offset 0, so every
device lookup failed. `IPC_REQ_SET_LNA` passed a scalar as the arg and read
`payload[1]`, so every call was refused. Same interface, both offsets, hours
apart — **one convention that must be remembered rather than seen**, not two
slips. The device session's framing, and it is the better one.

`IPC_REQ_GET_DEVICE_INFO` carries a comment reading "payload[0]: the arg byte,
unlike SET_PAIR_INIT above", which is the convention documenting itself one call
site at a time.

Make the wrong offset unrepresentable: carry the arg in its own field of
`ipc_msg_t` instead of in the payload, so a handler can neither read past it nor
short of it. Every new request type is another chance to get it wrong.

`Common/src/ipc.c`, `CM7/Core/Src/hubipc.c`.

### 31. The two sides' boundary lag disagree in a direction that cannot happen — `contract` `defect`

Neither `BEACON_BOUNDARY_LAG_US` nor `UPLINK_AIM_US` appears anywhere in
`radio_devices_docs/radio/`. **Silence cannot go stale**, which is why the
contract page never disagreed with either side and nobody found this by reading.

The hub measures superframe boundary to first bit directly: **358..366 µs over
529 beacons**. The device compiles **260 ± 5 µs** in `Core/Inc/beacon.h`, applied
as `at_us - BEACON_BOUNDARY_LAG_US` where `start_us` has already subtracted
`RADIO_PRE_SYNC_US`. So the device's constant covers the hub's term **plus** its
own detect-to-timestamp residual, and a residual cannot be negative — **the
device's number must be larger than the hub's and it is 100 µs smaller.**

One of three things is wrong: the hub's 358..366, the device's 260, or the
pre-sync subtraction. **Adopting either number would be the worst outcome
available** — the anchor moves 100 µs and may land correctly for a false reason.

The discriminator is agreed and cheap: the device forges a beacon on its own
boundary while the hub reports the lag it computes, so the device's own
transmit-path boundary-to-first-bit is measurable locally and the difference is
its receive residual with nothing else in it. To be run in the same boot as
item 30's single-slot window, so it does not become another two-window fraction.

**The pre-sync term is the suspect this side can contribute to.** It is
`RADIO_PRE_SYNC_AIR_US`, derived from `RADIO_US_PER_BYTE`, so it halved when the
rate did — 2 560 µs to 1 280. Any lag reading taken before the rate change and
compared with one taken after is out by a preamble and a sync word. Item 21's
`device synctime` run is exactly such a reading, and its re-measurement has a
prediction attached.

Raised by the device session. Device items 9 and 12.

### 33. Half rate is no longer forced by duty cycle — `debt` `contract`

`RADIO_DOWNLINK_EVERY` is 2 because beacon + downlink + join beacon every
superframe came to over 1% at 25 kbps. At 50 kbps the three are **16.0 ms,
0.800%**, and fit. The constraint that chose the value no longer binds.

**Nothing is wrong today** — the value stands and the assert that pins it passes.
What changed is that it is now a choice rather than a requirement, so a proposal
to raise the downlink rate has to be refused or accepted on its own merits and no
longer by pointing at the budget. The binding negative moved to the device: k = 3
uplinks every superframe is over 1%, which is why a device reports on a granted
cadence.

Listed so the next reader does not re-derive the old refusal from a page that no
longer says it.

`radio/phy.md` § duty cycle.

### 34. `RADIO_REPORT_FLAG_RESUMED` is on the wire and means nothing yet — `contract`

The bit is defined, `0x04`, and both firmwares compile it. **Neither sets it.**
The device session declined to set it rather than invent a meaning: it has two
different self-imposed waits — a `tx_gate` refusal and a `reserve_covers` refusal
— and picking one silently would have made the bit mean whatever it guessed.

The reader defines it, and the reader is the hub. Proposed, and awaiting the
device session's agreement: **the first transmission after a silence the device
imposed on itself**, whichever wait caused it. What the hub infers is the same
either way — that the silence was the device's own choice and is therefore not
evidence about the link. A reboot is not this; `uptime_s` carries that, and a
device that rebooted *and* served a reservation should say both.

**It cannot answer item 29 and must never be wired as though it could.** One
retrospective report carries it; if that report is lost the hub never learns.
Evidence when present, never evidence of absence.

`radio/tdma.md`, once agreed.

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

Nothing is implemented. `open_hub/network/tls.md`.

---

## Bench debts

### 27. Cold start is untested — `device`

Oscillator settling is temperature dependent and the sweep was at room
temperature. If devices are specified below freezing this needs re-measuring,
not assuming.

`radio/tdma.md` § lead time.

### 28. Owed to the device session

- Node B's PA-ramp capture.
- Node A (`0xC4D444AA`) has never been resynced.
- A constant for radiated energy in the duty-cycle model — **model only, never a
  roll-back**, so it must not be wired to anything that suppresses a transmit.
- A bracketed `timing` / `syncstats` / `timing` read over a named superframe
  window, so their `no-beacon` count has `beacon_n` as a denominator.
- **Warning before the next hub reset**, so node B is parked and instrumented
  when it happens. Park-and-wait recovery is built on their side; an unannounced
  reset spends the one honest test of it on nobody watching.
- **`link_v4` is superseded by `link_v5`** and their build includes the generated
  header directly. The sizes did not move, so nothing on their side fails to
  compile — only the version byte differs, and `aead_selftest` catches it at
  runtime here because a compile-time assert was added for exactly that. They
  need the new header before they build against the v5 wire.

`open_hub/testing/sdr.md`.
