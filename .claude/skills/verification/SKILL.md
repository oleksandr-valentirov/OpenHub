---
name: verification
description: How this project decides a claim is true - the five ways a check can be green and worthless, instrument discipline, test-vector discipline, and the sweeps that find defects in code that only runs when something is already wrong. Use when adding or reading a check, a self-test, a counter, a test vector or a probe; when a measurement is about to be quoted; and whenever a first success is imminent.
---

# Verification discipline

Every entry below cost this project a debugging session. They are not general
software advice - each one names the defect that produced it, because the shape
is what makes it recognisable next time.

Two repositories share these lessons: this hub and the WL55 device
(`../wl55_device`). **After any finding, check whether it applies on the other
side too.** Three times in one week a defect spotted in one repository was
sitting in the other - the append-point bug, the wire-length assert, the
self-test naming a moved contract. The sweep is cheap and the instinct is not
automatic.

## Five ways a check is green and worthless

- **Vacuous** - does nothing, looks like it does something. `sizeof` against
  itself; a length each side defines for itself. Fix: replace it.
- **Decorative** - computed correctly, nothing acts on the result. The flash
  store's exhausted-log path returned an error while the counter marched past
  the ceiling. Fix: wire the result to an action.
- **Name broader than coverage** - does real work, but reads like the general
  property so the next person stops looking. `hub_eph != hub_static` rejects one
  specific way of being unfresh and will be remembered as "the device checks the
  ephemeral is fresh". Fix: rename it honestly, never delete it.
- **Setup destroys the condition** - the give-away is ordering: the first
  statement undoes what the second was meant to observe. The device side checked
  that CRYP survives a hostile configuration as
  `gcm_open(...) == 0 && ecb_block(...) == 0`, and the GCM open re-initialises
  the whole config, so the ECB never saw the hostile state. Fix: **reorder**, not
  replace.

The fourth is the hardest to believe, because it is none of the other three: real
code, real inputs, something acts on the result, name matches intent. **Nothing
but mutation finds it.** Delete the defect's cause and watch the check stay green.

**Fifth, and structurally different from the four above: a check that does
exactly what it says, where what it says is insufficient.** The RX bandwidth
assert read `RADIO_RX_BANDWIDTH_HZ >= 2 * (DEVIATION + BITRATE / 2)` — Carson's
rule, correct, acted on, honestly named. It passed the 25 → 50 kbps change at
**exact equality**: 100 000 against 100 000. Nine of ten uplink frames then
failed CRC, because a filter exactly as wide as the signal has nothing left for
an off-centre carrier, and AFC was off.

The other four are checks that do not do what they appear to. This one did, and
certified the worst case as acceptable by its own terms. **An assert whose margin
term is missing passes hardest at the point of zero margin** — the value that
looks most like a deliberate design choice is the one where the check stops
carrying information. Every `>=` between a requirement and a resource is this
until the missing term is named: write the term, even as an allowance, and make
the comparison strict.

The reverse sweep is cheap: grep for `>=` and `<=` in asserts and ask what
happens at equality. If equality is the failure case, the operator is wrong.

## A guard pinned to one term of a product dies when the other term moves

`test_slots.c` asserted that `PAIR_RSP` **exceeds** a superframe's air allowance,
and the page beside it said why: so nobody could quietly "fix" the exception by
shrinking the frame. The frame never shrank. The **bit rate doubled**, the byte
took half as long, and the assert had to be inverted to `<=`. It was retired
correctly, by the one route it was not watching, and the reasoning it protected
was left to be rewritten by hand - which is the thing it existed to prevent.

Air time is `bytes x us_per_byte`. A guard on the product that is justified by an
argument about **one** factor survives only until the other factor moves. When
writing one, say which factor the argument is about, so the next change to the
other factor reads as a reason to revisit rather than as a routine fix.

**And the same change orphans every literal derived from it.** After the rate
moved, one sum - beacon + join beacon + downlink - was carried in three places at
two wrong values: the prose said 1.42%, a table on the same page said 0.74%, and
the comment on the assert that recomputes it every build also said 0.74%. The
true value was 0.800%. 0.74% was reconstructible as the downlink at **31 bytes**,
which `link_v4` had grown to 39 - so the stale figure names the change it failed
to follow. **A literal parked next to a live computation does not inherit its
updates**, and a comment beside a correct assert is the last place anyone looks.

The sweep, after any change to a shared constant: grep the documentation and the
comments for the old value **and for figures derived from it**, and prefer citing
the macro to restating the number.

## Know which artifact each assert pins

**A constant one side owns, that the other side's arithmetic needs, is the same
hazard as a contract number defined twice.** `UPLINK_AIM_US` is 700 us on the
device - the frame is centred in the slot's slack, not flush at its start - and
it appears nowhere in `Common/inc/` or in the hub. Hub-side reasoning about slot
occupancy has to assume a placement, and the assumption made here was flush at
slot start, which nothing implements. Both sides stayed internally consistent
while computing about different geometry. It cost a wrong guard analysis; the
expensive version is the device changing its aim point with every hub-side
occupancy calculation silently following the old one. **Where a frame sits
inside its slot is the same kind of number as where the slot sits inside the
superframe** - it belongs in the shared header.

"There is an assert" is not an answer. An assert tying two definitions *the same
side owns* pins nothing about a contract - the device had `PAIR_FRAME_LEN == 45`
while this side had 49, both asserts passing, both internally consistent, and
pairing that would have been refused on length with no diagnosable cause.

- `Common/inc/radio_protocol.h` - struct *and* literal in the shared header, so
  both firmwares compile the same number. **This is the one that matters for the
  wire.**
- `Common/inc/ipc.h` - payload sizes against `IPC_PAYLOAD_MAX`, one header both
  cores compile.
- `kvstore.c`, `keystore.c` - flash record sizes, correctly local: nobody else
  parses those bytes.

**An assert computed from design constants cannot see the implementation falling
short of them.** `radio_slots.h:175` guarantees the 1 s event deadline as
`EVENT_GAP + latch + air < 1000000`, and derives `EVENT_GAP` from
`RADIO_SLOT_OPPS = 3`. The device's own nonce guard made two of those three
opportunities unusable, so the reachable number was **one** and the true total
was 2011250 against a 1000000 budget — the requirement missed by a factor of
two, with correct arithmetic, real inputs, and nothing anywhere disagreeing.
**Every input to that assert was a number the design owns; none of them was a
number either firmware had to earn.** When an assert states a guarantee rather
than a relation between two definitions, ask which of its terms an
implementation could fail to deliver, and pin that one.

**Three asserts in one evening named a number nothing on that side used.**
`RADIO_RX_BANDWIDTH_HZ` is the hub's 125000 and the device configures 117300;
`RADIO_FRAME_AIR_US(28u)` guards "a downlink frame does not fit its region"
while `RADIO_DOWNLINK_BYTES` is 31; a roadmap entry computed 13 spare bytes from
a frame size no constant had. None produced a false green *yet*, and all three
would keep passing while the real number moved. **Grep for literals inside
asserts** — a literal in an assert is a number that has stopped tracking
whatever it was once equal to.

**A contract that changes width needs the width in its signature.** `uint8_t *fp`
is not a width and the compiler cannot see a too-small caller. `uint8_t fp[32]`
*is* - GCC diagnoses it through `-Wstringop-overread`, no `-Wall` needed, as long
as the caller's array size is visible. Keep a length parameter as well, for
callers that genuinely pass a pointer: two bounds catching two different callers.

**Assert the cost, not only the result.** `timing` counts beacons leaving later
than `RADIO_BEACON_LATE_LIMIT_US`. The device side's append-point bug was a page
erase on every write that *returned the correct answer* - no behavioural check
could see it, only a cost instrument. Verified non-vacuous by rebuilding CM4 with
the limit below the normal 4-9 us and watching it fire.

**A two-sided claim needs a two-sided falsifier.** A hypothesis that accepted
frames carry smaller carrier corrections than failures was pre-registered with
one test: *one accepted frame above 9521 Hz ends it*. It died the other way - a
**failure** at 1709 Hz, below an accepted frame at 4516 - and by the stated test
that would not have counted. Pre-registration is worth exactly what the named
falsifier covers, and a separation can break from either end. **Write the test
for the claim, not for the outcome you expect.**


## Instruments

**A reading that does not move can be the positive result, when the failure
mode predicts that it would.** The device's `tx.up` offset stayed at 590 us after
the transmit preamble went 4 -> 8 bytes, which reads as "nothing happened". The
offset is `(micros() - air - slot_at) + (air - on_air)`: `air` is measured, and
`on_air` is computed from the new length. Had the part ignored the register,
`air` would have stayed put while `on_air` grew, and the offset would have fallen
640 us to about -50. **The cancellation only happens if both terms moved as
modelled**, so an unchanged column witnessed the change on the antenna, from the
device's own timing, independently of the register read-back and of the far end.
Two limits worth stating whenever this shape appears: it was **post-hoc**, not a
check anyone set up, and it bounds the *total* frame length rather than the
preamble - the far end is still the only witness that the extra time has the
right shape.

**Two instruments that disagree can be jointly sound, and the disagreement can
be the measurement.** The hub read a device frame at +14 dBm as `-25 dBm, lna G1`
when 31 dB of step predicted -13. Level alone says "18 dB of a 31 dB step, the
column is broken"; gain alone says "G1 on every frame, the AGC never moves".
Both single-column readings are wrong. The next frame read `-13 dBm, G6` and
accepted, and the pair meant: **a front end in compression cannot report the
level of the signal compressing it**, so the under-reading and the frame
corruption were one event. The hub was one frame from sending "the column failed
its control" and retiring a working instrument. Every rule above says distrust
both until they reconcile; here reconciling them *was* the finding, and it was
only available because the two columns were logged **per frame, side by side**.
A cumulative version of either would have averaged the two states together.

**A gain or mode column is vacuous until one population holds two of its
values.** `lna G1` on every frame is indistinguishable from a constant until a
G6 appears beside it. Do not claim a categorical instrument works because it
prints a plausible category.

**An instrument that under-reports exactly when the fault is present inverts the
usual instinct.** A low level reads as "less signal than I thought" and sends you
looking for path loss, in the one condition where there is more signal than the
front end can take.

**An instrument that has never once passed cannot be read in either direction.**
`sync` read 0 for its whole existence; `device spiloop` failed 200 of 200 on
first run. A counter that has never been non-zero is indistinguishable from one
that *cannot* be, and a test that has never gone green is indistinguishable from
an invalid test - which is how the loopback got argued away half an hour before
it proved the fix. **Ship the control with the instrument**, not after it
disappoints: `spiloop`'s register arm is what made its FIFO arm readable.

**Say what an instrument has not yet been shown to do before reading anything out
of it**, and name the specific untested half. "This is new code, it might be
wrong" buys nothing. "This has never keyed up, so a silent hub is my bug before
it is yours" tells the other side which branch to attribute to whom, and one
window discharges it.

**An arrival-counting instrument is blind to non-arrival by construction.**
`device synctime`'s ladder compares `edges` against `frames` - two counts of
what *arrived* - so a device that stops transmitting produces zero of both and
the ladder stays green, correctly, forever. It validates the instrument, never
the link. What surfaced 98 consecutive missed cycles on the device was
`last_superframe`, a column added only because a timestamp wants to say which
superframe it belongs to: it records **when** rather than **whether**, and the
schedule position is the one property a silent transmitter changes. The device's
own `no-beacon` counter and stale marker were printing the answer to a console
nobody was reading; an outside observer with one extra column found it. **Make
every arrival carry the schedule position it should have had.**

**A counter can rise during the very event whose absence you are using it to
detect.** The device read "two beacons heard" as proof no quiesce was armed - and
the quiesce *announcement* is a beacon. Locally valid at every step, instrument
behaving exactly as designed.

**A counter can be correct about *how many* and useless about *when*.**
`rx_note_sync` in `CM4/Core/Src/radio.c` turns `SyncAddressMatch` from a level
into an edge correctly - `sync_was_set` is the guard that stops one arrival
being counted hundreds of times - but `flags1` is `RegIrqFlags1` read over SPI
in the superloop. So `sync_match` counts frames accurately at a resolution of
one superloop pass, and that period has never been measured. It is a frame
counter, not a clock, and a slot-position read out of it would report the poll
interval under the frame's name. Found only because the device side reported the
same shape in its own `wait_irq` and the sweep was run against this repo.

The fix is wired and unbuilt: **DIO3 is PC8, already `GPIO_MODE_IT_RISING` and
already dispatched** by `EXTI9_5_IRQHandler` - it fires today and does nothing.
With `SyncAddressMatch` mapped to it, TIM2 at 1 MHz gives a microsecond
timestamp against `superframe_start_tk`. Ship the control with it: **edges must
be >= frames accepted**, since every accepted frame had a sync match, and a
count below that means the mapping is wrong and every timestamp is noise.

**A control classified by the quantity it measures is not a control.** An
RTL-SDR envelope detector measured the hub's beacon 273 us longer than
`(payload + 11) * 320` predicted. The control - the same detector pointed at the
device's SX126x, a different vendor - agreed to within 15 us, and it was not the
device at all: the bursts were the hub's own downlink. They were labelled "node
B" **because their duration matched node B's expected frame length**, and that
duration was then reported as an independent measurement of node B.

The label came from the quantity being measured, so the test could only agree.
Two other properties were in the same capture and either would have refused it
in one line - the bursts sat 25.17 ms after a beacon (`RADIO_DOWNLINK_OFFSET_US`)
on a 4.00 s cadence (the half-rate downlink), where node B's slot-1 uplink is at
69 ms on a 16 s cadence. It survived because `radio_downlink_t` and
`radio_uplink_t` are both 31 bytes, so the payload divisor was right and every
number came out plausible. **Classify by a property independent of the result,
and check the cadence and position of anything identified by its size.**

**And "stable across thresholds" does not mean "not a ramp".** The excess was
argued not to be PA ramp because sweeping the detector from 30% to 70% of peak
moved the duration by 9 us. A ramp that reaches near-full amplitude quickly and
then holds is threshold-insensitive across that range *and still adds time at
both edges*: the thresholds all fire within a few us of each other, on an edge
that began 130 us earlier. Threshold stability says the edge is steep, not that
it is absent - and it was the sentence that made a wrong number credible.

**Before believing a negative, run a control**: push through the same path a
frame you know decodes. Six false negatives on this bench so far, every one in
the tools rather than the firmware.

**Any probe needs its negative case exercised once.** `make -q check` on a
`.PHONY` target returns 1 unconditionally, so it cannot distinguish its two
outcomes and the verification never happened. The forms that mean something all
look the same: `make -q` against a real target reading 0/1/0 across
settle-touch-rebuild; a mutation that must fail; a test length outside a defect's
class that must pass.

**A guard can fail in the direction that guarantees the thing it prevents.**
`ps -eo pid,cmd | grep "[w]inhold"`, meant to stop a second window-holder
starting, **matched its own shell** - the wrapper's argv contains the whole
script, pattern included. It printed "ALREADY RUNNING", started nothing, and the
pairing window expired in silence. The bracket trick defeats grep matching itself
and does nothing about the shell around it. Match on the process *name* as well:
`$2 ~ /python/ && /winhold\.py/`. The `pkill -f` form had already cost two exits
the same session: **fixing one instance of a hazard is not fixing the hazard.**

Nor is writing it down. On 2026-08-21 both benches hit it again within minutes of
each other, on the two halves of one measurement, having read this page:
`pkill -f per_run.py` matched the shell whose argv held the pattern and killed the
poller, its replacement and itself. So the rule is mechanical rather than
cautionary: **never match a pattern the matching command's own argv contains.**
Kill by a PID the process recorded when it started, and have any poller worth
trusting write one. Both failures were in the instrument built to make a
measurement window unambiguous, and neither was in firmware.

**A counter's heading can scope it more narrowly than the counter is scoped.**
`rx_crc_err` is global and is incremented from the uplink path and the join path
alike, and it prints as `rx (this window)` under the *pairing* command. Reading
`devices` — which prints `uplink N seen, N ok` and every uplink refusal bucket —
showed nine frames arriving and one accepted with **nothing named as the
difference**, and produced a report to the other bench that seven frames were in
no bucket at all. The bucket was one command away.

Four instances in one day, both benches: a `gfsk 25 kbps` literal beside a modem
keyed at 50, a `sync` field carrying `state` while the device was out hunting for
a beacon, a conventions check green on a tree that did not contain the commit,
and this. **None of the four was a wrong number.** Each was a true sentence about
something adjacent to what the reader would take it for. When a report is going
to another party, state what the number is counted over, not only what it counts.

**`cd X && start & echo $! > pid` records the wrong process, in the wrong
directory.** `A && B & C` parses as `(A && B) &` followed by `C`, so the `cd` and
the launch went into a background subshell while the `echo` ran in the foreground
shell - writing the subshell's pid into the repository root instead of the
launcher's pid into the scratchpad. The kill that followed found no file, killed
nothing, and left two pollers fighting over one serial port: the exact contention
the pid file existed to prevent. **Group the launch explicitly**, and check the
pid file exists before trusting a kill that reads it.

**A guard one level up can make a whole branch unreachable, and the branch still
reads as working code.** `devices rate <n>` was documented in the usage string,
described in the roadmap and implemented in `cmd_devices` — and the command table
declared `{"devices", 0, 1, ...}`, arguments after the name, so the dispatcher
rejected two of them before the handler ran. The branch had never executed once.
Nothing about the handler looks wrong, because nothing about it *is* wrong. **Ask
what admits a call before asking what it does**, and run every documented command
once: a printed usage line is not evidence that the form it describes is
reachable.

**An arbiter with one caller is not an arbiter.** The device's `tx_gate` existed
to decide which superframes were spoken for and was wired to the exchange path
only, so every uplink went around it and the reporting loop and the CLI could
seal two plaintexts under one GCM nonce. The gate was not missing, not broken,
and passed every test it had - it was on the wrong path. Ask what fraction of
the calls a guard actually sees, not whether it exists.

## A discriminator that cannot discriminate

Before proposing a test, check that the two hypotheses actually predict different
outcomes under it. Twenty-one uplinks produced ten sync matches at the hub, and
the device's channel list split **exactly ten and eleven** on whether
`hop_to_grid` skips the reserved join channel - which looked decisive. It was
not: `hop == grid` is true precisely when `grid < 14`, so that split is also the
low and high halves of the band. **A convention disagreement and a
frequency-selective receiver predict the identical partition**, and no comparison
of those lists could ever separate them.

Worse than a wrong hypothesis, because it produces a confident answer either way.
The give-away is that the discriminating variable was never varied
independently - it was derived from the same index as the thing it was meant to
distinguish. The repeat run fixed it by accident: the hop map spread the twenty-
one frames across both halves, making band and settling time independent in one
population.

It happened twice in one evening, in both directions, which is what makes it a
class rather than a slip. The second was a *measurement* rather than a proposed
test: three burst types were compared for carrier offset, and the one that came
back furthest off was **both the shortest burst and the earliest after a
retune** - while the estimator's own bias was known to grow as bursts get
shorter. Settling time and burst length moved together across the only pair that
carried the claim. The comparison that survived was the one between two bursts
of **equal length**, which is the control the first comparison never had.

**And check the instrument you are asking the other side to read exists.** The
test proposed was "compare which superframes produced your ten matches" - to a
counter that counts and does not log. A count has no set to match. That is the
same rule as making every record carry its schedule position, failing from the
asking end rather than the emitting end.

## Windows, brackets and the arming that writes your answer for you

**A declared window whose contents are still growing is a denominator with a
timestamp.** Two benches exchanged transmit lists all night; one was sent while
its window was still running, the other side built a fraction on it, and 7 of 7
was really 7 of 9. Neither half was wrong. The list was accurate when written and
the counters were accurate when read, and they described populations separated by
minutes rather than by build. **Every list carries the superframe it was current
as of**, and a fraction assembled from someone else's list is quoted with the
span its reads actually covered.

**A bracket is not closed until you have checked it stopped moving.** A window
was declared closed at superframe 569904 and a read at 569935 was about to be
reported as its total. A second read at 569979 came back one sync higher — a
thirteenth transmission the sender had not listed. Reporting from the single read
would have put an extra frame in the window and moved the interval in the
direction that flattered the conclusion. **The confirming read costs one command
and is the only thing that can tell a closed window from a quiet one.**

**An experiment's own arming can manufacture the pattern you then read out of the
data.** Two accepted frames landed on grid 13 and grid 15, either side of the
join channel at 14, and that symmetry got a paragraph and a mechanism before
anyone asked how the windows had been constrained. The band filter split at
exactly 14: one frame came from a window allowing 15..28 and the other from a
window allowing 0..13. **Half the pattern was the experiment design showing
through.** What survived was narrower and better - within each constrained
population the model named a unique winner and got both - but the geometry that
made it look profound was drawn by the filter. Ask what the collection excluded
before reading a shape out of what it kept.

## A statistic's direction must be an argument, not an assumption

Both sessions independently wrote the same one-sided Fisher helper, summing the
hypergeometric tail **upward** from the observed cell. That is correct for "is
this arm higher" and silently wrong for "is this arm lower", which returns a
p near 1.000 for a perfect separation. It went unnoticed for a night because
every test until then happened to ask "higher"; it was caught only because
p = 1.000 on an obvious-looking split was absurd enough to re-read.

**A helper that cannot fail visibly will be quoted.** The fix is not care - it is
to make the direction a parameter with no default, so a caller asking the wrong
question cannot be silently answered. The same shape covers any test whose
alternative hypothesis lives in the caller's head: one-sided anything, a
threshold comparison, a monotonicity check.

Two consequences when such a bug is found: **say which results the broken
direction touched**, because "the tool was wrong" otherwise invites re-checking
everything; and prefer **two independent implementations agreeing** to one
implementation being careful - the agreement is what rules out a shared bug, and
it only works if the implementations were not copied from each other.

## A sentence no instrument on your side can contradict

**Prose does not fail on its own. It fails where nothing local could have
refuted it.** The hub wrote *"at -17 dBm I am accepting essentially everything
that arrives, so delivered is close to transmitted"* and spent two rounds
hunting a receiver ceiling that did not exist. `accepted / delivered` was 81 %.
`delivered / transmitted` was 31 %. Both fractions were true and only one of them
had both halves on this side - **the transmitted count does not exist on the
receiving end at all**, so no counter, assert or console line could have
collided with the sentence. It was locally unfalsifiable, and that is a property
of the instrumentation rather than of the writing.

The fix is not to write more carefully. It is to make the wrong sentence hit a
printed number:

- **the observed cadence beside the granted one**, so what the far end does sits
  next to what this end asked for;
- **a counter on every silent recovery**, so a path that quietly repairs itself
  stops being invisible;
- and, where the denominator genuinely lives on the other side, **ask for it** -
  the device session counted 291 transmits over a named superframe window and
  turned a ratio into a delivery rate in one message.

What no counter reaches is quoting one true fraction where another was meant.
**Naming the denominator is discipline; making the sentence collidable is
engineering.** Both are needed and they are different work.

**And an interval distribution can recover the denominator without the far end.**
`cadence: grant 8, seen every 2 (mean 4.9 over 27 gaps)` - the **minimum** gap is
the cadence, the **mean** is the same cadence with missed cycles folded in, so
`min / mean` estimates delivery: 0.41 against 0.36 measured from the device's own
count. Nothing is derived from the grant, which is what item 4 forbids; the
minimum is observed.

Its limit is exactly where it would matter most: **the minimum is only the
cadence if one pair of adjacent cycles ever arrived.** At low delivery no
adjacent pair lands, the minimum reads a multiple of the true period, and the
estimate comes back **too optimistic**. An instrument that degrades toward good
news needs the count from the far end before it is quoted.

## Numbers are fractions, and both halves need the same population

Four times in one evening a numerator and a denominator each individually correct
were combined across different windows, runs or builds: nine syncs against a
transmit log that had stopped being written; a loopback's *passes* against a
frame's *bytes*; three hop channels quoted with a key read from a different boot.
**Each half survives any check applied to it separately**, so nothing catches it
but asking what the denominator is a fraction of. The worst produced not a wrong
answer but a wrong *dismissal* - a working instrument argued out of the evidence
pile, and a bad conclusion at least gets tested.

**A statistic about the inputs to an average is not a statistic about the
average.** `timing` prints `spread +3107..+5220 ppm`, the scatter of individual
LSE calibration *windows*, and it was quoted as though it were the scale in
force - 2113 ppm on a 72.5 ms offset is 153 us, which would have swallowed every
timing difference either side had measured. The scale actually applied is the
average of many windows: measured over 27 samples it had **sd 12.6 ppm, worth
2.8 us**. The project's own timebase page already said the mean matched a host
clock to 51 ppm while a single window carried ~350, and the two numbers were
combined anyway.

The fix was to make every offset carry the ppm that converted it, and the
falsification came on its first run: raw-tick spread and microsecond spread came
back at **sd 37.0 against 36.8, ratio 0.99**, so the conversion contributes
nothing within a run and the jitter is real. **The instrument built to test the
hypothesis killed it**, which is what an instrument is for. What survived was
smaller and in the other direction: the applied scale moved ~273 ppm *between*
runs, about 20 us, so between-run means need the ppm reported beside them and
runs recorded before the fix cannot be corrected retrospectively.

**A filter you armed can manufacture the pattern you then read out of the
data.** Two accepted frames landed on grid 13 and grid 15, either side of the
join channel at 14, and a mechanism assembled itself instantly: channel maps
agreeing at the centre and diverging outwards. But the two frames came from
windows filtered `band low` and `band high`, and that filter splits the grid
**exactly at 14** - so one frame from each half is "either side of the join
channel" by construction. Half the pattern was the experiment's own arming. What
survived was narrower and had to be restated with its odds. **Ask how the
population was constrained before reading a geometry out of which members
appeared in it.**

**The denominator can be a contract nobody implemented.** `uplink windows 375,
sync 1` was read as eleven missing reports because the grant says `report_every 8`
and 182 s is eleven of those. The device had no reporting loop at all — `uplink_build` had two callers, both
the CLI — so the population was one transmission, one seen, one ok. `windows` counts *this side's
opportunities*; the device's transmissions are the population that matters, and
the two have no relationship.

**And the instrument that would have prevented it would have made it worse.** An
`expected` column on `devices`, derived from the grant and superframes elapsed,
would have printed **11** next to `received 1` - promoting an assumption to a
measurement and making the wrong dismissal look confirmed. The hub grants
`report_every` and has no evidence any device honours it, so "reporting as
granted" and "has no scheduler" are indistinguishable from here. **A derived
denominator is the assumption with a column heading.** Do not add it; if a
device's cadence needs checking, the device must say what it sent.

**A guard stricter than the contract is green forever and removes an option.**
The device's uplink nonce guard refused any superframe it had already sealed
under, while the nonce is `(superframe, dev_id, direction, slot)` and the slot is
what makes the three k=3 opportunities distinct. So the device could use one
opportunity of three, refusing its own second and third **after** deciding to
send them - and it had done so silently since it was written, because a
too-strict rule never fails visibly, it just makes something impossible. The hub
had the same rule keyed on the pair and was correct. **Conservative is not a
synonym for safe**: this one removed the exact instrument that later separated a
receiver's own spread from the channel's.

## Test vectors

**Round-tripping tests the round trip, not the assembly.** Seal-then-open agrees
with itself under any self-consistent nonce, AAD or key packing. Only matching a
*published* frame proves the assembly, which is where the wire's little-endian
struct fields meet the crypto's big-endian inputs. Both endiannesses are live
inside one frame: `hub_id` is `33 44 22 11` in the GCM salt and `11 22 44 33` in
the header of the frame carrying it. Not fixable without breaking a beacon decode
verified on air, so whole frames are pinned in `Common/test/vectors/pair_v2.txt`
and nobody has to infer which rule applies.

**A vector whose consumer does not exist is untested in the way that matters.**
`key_hop_gen0` reproduced byte for byte for weeks and proved only that HKDF
works - nothing consumed it, so nothing could disagree. That is how the
pairwise-hop-key defect survived cross-verification by two independent
implementations. The converse holds: a vector whose consumer exists but whose
*primitive* is never compared against an outside answer is equally blind, which
is what let the hop PRF encrypt four bytes of sixteen.

**A vector set with one instance of a field tests its format and never its
source.** `pair_v2.txt` carries a single `pair_req_superframe`, so inside it the
request's superframe, the live counter and the last beacon's are the same number,
and an implementation reading any of the three reproduces the vector forever.
A field whose provenance matters needs **three distinct values**, one per
plausible source. `Common/test/vectors/pair_prov` is that set, and it publishes
what each *wrong* read produces, so a failure names the source instead of
reporting a mismatch.

**Prefer deleting the wrong value to testing for it.** The device side had the
superframe as a parameter to two consumers and updated one; the fix was not a
test but making both read the same struct field, so **there is no second value in
scope to pass by mistake**. A test finds a caller passing the wrong value;
removing the parameter means there is no wrong value to pass.

**When a test length is chosen to catch a known defect, check that a length
outside the defect's class still passes.** The 19-byte grant exists to trip the
GCM partial-final-word bug; the 8-byte uplink report exists to stay green while
it trips. A test failing on both is reporting something else.

**A digest over a vector file must cover the values, not the text.** All three
generators hashed their emitted output, so a reworded comment moved the digest of
a set whose values could not have changed. `value_digest()` sorts by key and
drops comments; the rewrite guard still compares whole text, because the guard
exists to make someone think and the digest exists to tell consumers whether
anything they depend on moved. Prose corrections do not need a new version.

**A digest in a generated artifact detects regeneration, never tampering.** The
value is a literal baked into the file it describes, so a hand-edited header
keeps its old digest and every consumer compiles the tampered values and agrees.
Measured: editing two bytes of `HV_DECK0` leaves `vectors` reading `ok` on both
cores and fails the host deck test on the first assertion. **The values are the
check; the digest is a label.** `vectors` therefore prints what it covers on
every run, because an operator who reads "ok" twice will otherwise carry away
that the vectors were verified.

**A value correct for a *different* computation reads as reassuringly familiar.**
`5e102fa24f59aaf3` was hop_v1's text digest and there is no path back to it under
a value digest, but it looks like the right answer. Same shape as a verification
reported against a commit that predates the file, and as a "FIPS-197 constant"
actually produced by the same library it was meant to check.

## Reports, and the post-condition rule

**And a report can contain a reading that was never taken at all.** Three
counters print on one line - `sync`, `crc err`, `frames`. A poll log was filtered
down to the AFC lines alone, and the counter line was then written into a message
from what the filtered lines implied rather than from the log: "`sync 2, crc err
2, frames 0`", both frames corrupt. The real line was `sync 2, crc err 1,
frames 1` - **an accepted frame, reported as its opposite**, on the one column the
other side had asked to watch.

Nothing about it looked like a guess. It had the instrument's exact format, it
was internally consistent, and it agreed with the expectation that produced it.
**A prediction written in a measurement's format is indistinguishable from a
measurement at the receiving end.** When a filter drops columns, the dropped
columns are not available to the sentence either: go back to the log, or say
which columns the read covered.

**A sentence in a measurement's format was not necessarily a measurement.** The
hub session wrote `sync 2, crc err 2, frames 0` having extracted only the AFC
lines from its poll log and composed the rx line from what it expected to
follow. The true reading was `crc err 1, frames 1` - **the opposite on the one
column the other side had asked it to watch**. Same failure on this side an hour
earlier: "nine below grid 14 and twelve above", stated while listing eleven and
ten, and built on twice before anyone recounted. The receiving side cannot tell a
read number from a predicted one, because both arrive in the same format.
**Paste the tool's output, or label the value derived** - the same session later
reconstructed an unprinted number from a truncated mean, showed the bound
(`6587..6597`), showed the method reproducing five known inputs exactly, and
called it derived. That one was worth having.

**A falsifier must have the same shape as the claim.** The hub pre-registered
"one accepted frame above 9521 Hz ends it" for the claim that accepted frames
carry *smaller* corrections than failures. That is a separation, which can break
from either end — and the test watched only the end the author expected. It died
from the other: a failure at 1709 Hz below an accepted frame at 4516. **By its
own stated falsifier the refutation would not have counted.** Same class as an
arrival-counting instrument that cannot see non-arrival: the test could only ever
catch half of what the claim asserted. Before registering one, state the claim as
a shape — a separation, a bound, an ordering — and check the test can fail
everywhere the shape can.

**A threshold named in advance still needs to be able to pay out.** This side
pre-registered "six frames, zero syncs, and the rate is refuted", then computed
afterwards that six silent frames happen 13% of the time at the rate in
question. Pre-registration defeats picking the answer after the fact; it does
**nothing** about naming a threshold too small to resolve the effect. Both
sessions had to walk it back, and only because the numerator turned out wrong
for unrelated reasons. **Do the power calculation before the experiment, not
after the disappointment.**

**A report of a verification can be broader than the verification.** The device
side ran `git checkout -- <file>`, `git status` and a grep, then wrote
"byte-identical to `5f7d784`" - naming a commit it had not compared against, which
predated the file. The check was real; the sentence was not, and the receiving end
cannot tell the two apart. **Say what was actually run**, because that sentence
cannot be broader than the check - it *is* the check.

**After verifying a check, confirm it reached HEAD.**

```bash
git grep -n _Static_assert HEAD -- Common/inc CM4 CM7
```

The wire-size asserts in `radio_protocol.h` were written, verified non-vacuous by
breaking them, then destroyed by the cleanup that undid that test -
`git checkout <file>` reverts to HEAD and takes uncommitted work with it. The
commit that followed claimed they were there. Avoiding that one command is not
the fix: a stash, a failed rebase or an editor revert all end in the same place.
**Check the post-condition, not the procedure** - only HEAD can disagree with the
commit message.

**`git checkout` discipline attaches to the operation, not the file type.** Both
sides used scratchpad copies religiously for C files all afternoon and reached
for `git checkout --` the moment the file was Python or a generator; the device
side lost uncommitted work to it in the exact command this rule warns about. It
was invisible in the tree, in the run and in the output, and found only by
grepping for a symbol expected to be there.

**A decision record can be correct, agreed, and unimplemented**, with nothing in
either firmware disagreeing with it. ADR-0021 said the invitation replaces the
join beacon; both sides agreed; the replacement was never written. **The document
is not a check.**

## The two sweeps

**The failure-path sweep.** Five defects between the two repos lived in code that
only runs when something is already wrong: a guard whose refusal nothing acted on,
a discarded return, a half-guard reading as a whole one. Grep for the *pattern* -
discarded returns, `(void)` calls, guards with no matching enforcement, positions
derived from a validity-filtered scan - rather than re-reading the module. Do it
when adding anything with an error path, and **sweep for the class, not the
instance**: the append-point bug existed in both stores and was fixed in one of
them an hour before anyone thought to check the other.

**The success-path sweep, which is the same thing inverted.** Five defects found
the evening pairing first worked lived in code that could only execute once
something finally went *right*: `GET_DEVICE_INFO` read its index from the wrong
byte and had failed every call since it was written, because printing a row needs
a paired device; the device side's grant was never persisted; its `hop` command
answered with the test-vector key. **Grep for the paths only a success unlocks
whenever a first success is imminent** - that is the cheapest moment they will
ever be found.

## Faults that hide by being constant

**A fault with a duty cycle of 100% looks like a property, not an event.**
`PAIR_INIT` fires every 4th superframe and the join beacon every 2nd, so every
invitation shared a superframe with a beacon - not sometimes, always - both keyed
at `join_offset_tk` about 8 ms apart. The device heard 15 beacons and zero
invitations in one 59 s window and read the perfect regularity as evidence of a
systematic fault, which it was: radiated correctly, on the right carrier, into the
shadow of the frame in front of it. **Everything intermittent announces itself by
working sometimes; this could not.**

**A register read back off the part is evidence about the antenna; a function's
return value is evidence about the function.** Nothing on the transmitting side
could see the above - `RegFrf` reads back 866.5 MHz *after* the transmit,
`PacketSent` is observed, `tx_err` is zero, `sent` counts up. Reading `RegFrf`
after the frame went out is what eliminated the carrier hypothesis in one
measurement instead of an hour of argument.

**A send counter cannot see a wrong carrier.** `pair_tx()` tuned the join channel
itself, deliberately, to remove a dependency on how the exchange got there.
Reused for the downlink it kept doing that, and 93 frames went out on 866.5 MHz
while the device listened on a hop channel - `sent`, `tx_err` and `opportunities`
all correct, because the frames really did transmit. It took the device naming
the grid slots it had listened on. **A function whose name is its caller
(`pair_tx`) rather than its contract will be read as the general verb**; make the
varying thing an argument.
