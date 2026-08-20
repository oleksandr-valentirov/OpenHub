# Pairing and the quiesce

**Status: the schedule is implemented and measured on air. The key exchange is
implemented on the hub and verified against `pair_v2` on hardware; it has not
yet run against a real device.** Both halves of the wire contract were written
from the specification independently and agree byte for byte.

## The problem

The hub has one radio. Pairing happens on the [fixed join channel](joining.md)
at 866.5 MHz; data happens on whichever of the 28 hopping channels the
[superframe counter](hopping.md) selects. The hub cannot be on both.

Beaconing is not the hard part — a join beacon is 8 ms and slots into the
schedule like any other frame. **Receiving** is. The hub does not know when a
joining device will answer, the exchange has compute pauses of tens of
milliseconds on both sides ([ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md)),
and every millisecond spent listening on 866.5 MHz is a millisecond the hub is
deaf to its own network.

## Two phases, not one mode

The obvious design is a pairing mode: announce it, stop the grid, pair, restart.
That is right about the exchange and wrong about the window, because the two have
completely different durations.

`device add <id> <fingerprint>` opens a window for **60 seconds**, and almost all of that is spent
waiting for a human to press a button on a device. The exchange itself is a
handful of frames. Suspending the network for the waiting time buys nothing and
costs 60 s of service every time an operator opens a window — including every
time the device never shows up.

So the window and the suspension are separated:

| Phase | Duration | Grid | Trigger |
|---|---|---|---|
| `LISTEN` | up to 60 s | **running normally** | operator: `device add <id> <fingerprint>` |
| `QUIESCE` | 4 superframes, 8 s | **suspended** | a valid `PAIR_REQ` arrives |

**The quiesce is triggered by the device, not by the operator.** The network goes
quiet only when a pairing is actually happening, and for 8 seconds rather than
60.

```
                  device add <id>              valid PAIR_REQ
     RADIO_PAIR_IDLE ------> RADIO_PAIR_LISTEN ------> RADIO_PAIR_QUIESCE
            ^                     |    ^                        |
            |    window expires   |    |   resume superframe    |
            +---------------------+    +------------------------+
```

## LISTEN: the join region

During a window the grid keeps running and the hub adds a **join region** at
1 874 000 µs into each superframe — the tail of the uplink region, past slot 95.
It retunes to 866.5 MHz, transmits a join beacon every second superframe, listens
for 100 ms, and retunes back with 126 ms to spare before the next boundary.

The region **overlays** the uplink tail rather than being reserved permanently.
Slots are assigned from 0 upward, so with fewer than 96 devices it sits over
nothing. A permanently reserved region would cost every superframe forever to
serve something that happens when a human presses a button.

The 100 ms receive window is split across superloop passes rather than blocked
on. Sitting in one iteration for 100 ms would straddle a superframe boundary and
put 100 ms of jitter into the beacon **every device measures its period from** —
see [tdma.md](tdma.md#the-beacon-must-leave-at-a-fixed-offset-and-does).
Measured with the window open, the beacon still leaves 1–4 µs late.

## QUIESCE: what the hub promises

The announcement rides in the **data beacon**, which every paired device already
receives every superframe. A separate announcement frame would cost duty cycle
every superframe to say nothing almost every time.

```c
typedef struct radio_data_beacon {
    uint8_t  type;
    uint8_t  version;       /* now 2 */
    uint16_t net_id;
    uint32_t hub_id;
    uint32_t superframe;
    uint8_t  flags;         /* RADIO_BEACON_FLAG_QUIESCE */
    uint8_t  resume_in;     /* superframes until normal traffic resumes */
} __attribute__((packed)) radio_data_beacon_t;
```

### Why a countdown and not a flag

**"Stand down and wait for the next broadcast" is the one rule that cannot be
written that way.** The announcement is the *last frame a device will hear* — the
hub is about to be on another channel. A device told only "quiescing" has to stay
in receive to find out when that ended, which is the opposite of standing down.
It would burn 8 seconds of receiver current per pairing, on 64 nodes, to learn
something the hub already knew when it spoke.

`resume_in` makes the announcement self-contained:

```
resume_at = superframe + resume_in
```

The device computes that once and sleeps. It wakes into the superframe the hub
promised and finds the hub there.

### The announcement is repeated

`RADIO_QUIESCE_ANNOUNCE = 2`: the announcement goes out on the first two
superframes of the quiesce, `resume_in` counting down so both copies name the
same absolute superframe.

One copy is one lost frame away from a device that wakes into its slot and
transmits at a hub that is not listening. It was written with one copy first,
and the gap was found by the device-side implementer asking whether `resume_in`
was a countdown or a constant — a question about the contract, not about the
code.

It earns the constant twice. The device rejects a quiesce announcement that
arrives on the first beacon after a *rejected* one, so a corrupt or forged frame
immediately before an announce run consumes the first copy. With one copy that
silently costs a legitimate quiesce; with two it costs nothing.

**The announce run is not free time.** The hub is still beaconing on the hop
channels during it, so `RADIO_QUIESCE_SUPERFRAMES - RADIO_QUIESCE_ANNOUNCE` is
what the exchange actually gets with the hub parked on the join channel: 2
superframes, about 4 s. `Common/test/test_slots.c` asserts `SUPERFRAMES >
ANNOUNCE`, because a quiesce that is all announcement gives the exchange nothing.

### Four superframes, and now the arithmetic behind it

The announce run takes two, so the exchange gets **two clear superframes — 4 s**
with the hub parked on the join channel. What has to fit:

| | |
|---|---|
| hub P-256, one pairing's worth | **330 ms, measured** |
| hub derivation end to end (2 mul + 4 HKDF + 2 HMAC) | **493 ms, measured** |
| device P-256, two scalar multiplications | ~205 ms (2 × 102.6, measured) |
| device point decompression | 13 ms |
| four frames of air time | **75.5 ms**, computed |

About 800 ms against 4000. The margin is for round trips and retries, not for
computation.

Every air-time figure on this page is `RADIO_AIRTIME_US` of the frame size that
`radio_protocol.h` asserts, not a number carried forward by hand. Three of them
had drifted low at once, and all three drifted in the same direction:

| Frame | Bytes | Air |
|---|---|---|
| `PAIR_REQ` | 57 | 21.76 ms |
| `PAIR_RSP` | 59 | 22.40 ms |
| `PAIR_CONF` | 26 | 11.84 ms |
| `PAIR_ACCEPT` | 50 | 19.52 ms |
| hub total | | **41.92 ms** |
| device total | | **33.60 ms** |

### The budget belongs to a radio, not to a conversation

`RADIO_PAIR_EXCHANGE_US` used to sum all four frames and compare the total
against the hub's allowance — charging the hub for the two frames the *device*
transmits. Real arithmetic on the wrong quantity: it overcounted by 33.6 ms and
would have fired for a device-side change that is none of the hub's business.
It is now `RADIO_PAIR_HUB_AIR_US` and `RADIO_PAIR_DEV_AIR_US`, each against its
own transmitter. Raised by the device side, which is where the overcount was
visible.

**Three denominators, all three asserted.** Across the superframes the hub is
actually parked on the join channel — `RADIO_QUIESCE_SUPERFRAMES` minus the
announce run, so two — 41.92 ms in 4 s is **1.048%**, over the per-superframe
design budget. Averaged over the whole quiesce it is 0.52%. Against ETSI's
one-hour observation period one pairing is 0.0012%.

Only the first of those is above 1%, and the per-superframe budget is this
project's own rule rather than the regulation. Asserting only the comfortable
denominator would let the exception be defended by arithmetic instead of by
reasoning — and shrinking a frame to pass a limit that does not apply is exactly
the "fix" the two-direction assert exists to prevent.

**The exchange is outside the steady-state air budget, deliberately.** `PAIR_RSP`
is 59 bytes — **22.4 ms on air, more than the whole 20 ms a normal superframe
allows**. That is not a defect: during a quiesce the hub is parked on the join
channel and beacons nothing, so the superframe it lands in has none of its usual
traffic. The whole exchange is 65.9 ms across four superframes, 0.8%.

Written down because a duty-cycle governor comparing one frame against
`RADIO_AIR_BUDGET_US` would flag this and be wrong, and because
`Common/test/test_slots.c` asserts **both** directions — that the frame exceeds a
superframe's allowance and that the exchange fits the quiesce. Checking only the
second would let someone "fix" the first by shrinking a frame and quietly lose
the reason the exception is allowed.

Raised by the device side, whose 256-byte radio buffer gives it no opinion on
these frame sizes at all — so it came from the shared air budget rather than from
either radio's framing, which is the only place a hub-only constraint could have
been caught from outside.

The hub figure is measured directly by the `p-256 hub pairing share` self-test,
which does exactly one ephemeral keypair, one peer-key validation and one shared
secret against a fixed peer public key — the hub's real work for one pairing and
nothing else. **330 ms.**

It exists because the first version of this table used a derived number, and the
derivation was then mis-stated: the combined `p-256 ecdh+validate` case runs
**four** scalar multiplications and two validations in 669 ms, and 669 was
compared against the device's **single** 102.6 ms multiplication to claim a 6.5×
gap. Per operation it is **1.6×** — 167 ms against 102.6. The budget rows had
always been right; only the headline was wrong. A number that has to be divided
before it means anything is a number that will be divided wrongly, so the
division was replaced by a measurement.

The direction survives and is the part worth keeping: 167 ms of software P-256
on a 480 MHz Cortex-M7 really is slower than 102.6 ms of PKA on a 48 MHz
Cortex-M4, so **the hub is the side worth counting** — which is the opposite of
what "the hub is the big processor" suggests. At 1.6× that is a mild asymmetry
and plainly needs no offloading; at the mistaken 6.5× someone might have
concluded otherwise.

**This rules out three superframes**, which would leave one clear superframe —
2 s against ~650 ms of compute plus every round trip, with no room for a retry.

### The resume superframe is a promise, not a plan

Once announced, `resume_at` is never moved. It is chosen at announce time and the
hub resumes on it whether or not the exchange finished. An exchange that overruns
loses its window; the network does not lose its schedule.

This is not a detail. Sixty-four devices may be asleep against that number, none
of them listening. Extending a quiesce would strand every one of them.

### The counter does not stop

**A quiesce suspends transmission, not time.** The superframe counter keeps
advancing through it.

That is what makes the whole thing cheap. The hop sequence is *indexed* by the
counter rather than stepped by it
([ADR-0008](../decisions/0008-keyed-shuffle-hopping.md)), so a device that slept
through the silence computes the channel for the superframe it wakes into and is
simply back. **No resynchronisation, no rejoin, nothing to fast-forward.** The
stateless hop design was chosen for sleeping devices and pays for this for free.

Freezing the counter instead would repeat superframe numbers, and once frames are
sealed a repeated counter is [nonce reuse](../security/wire-crypto.md#nonce-construction).

## The denial-of-service question

**The data beacon is not authenticated today, so the quiesce flag is a denial-of-
service primitive.** Stated plainly because it is the sharpest edge in this
design: anyone who can transmit a beacon can silence the network.

Three things bound it, and none of them is a cryptographic guarantee:

- **A forger must be on the right hop channel at the right superframe.** The hop
  sequence is keyed, so an attacker who does not have the hop key transmits where
  nobody is listening. This is a real practical barrier and it is not a proof.
- **Both ends clamp `resume_in` to 4 superframes.** The hub will not announce
  more and a device must not honour more. Matching values mean a forged beacon
  cannot buy more downtime than an honest one.
- **A quiesced device stands down from transmitting; it does not go deaf
  forever.** The clamp bounds the worst case at 8 seconds per forged frame, and
  sustaining it means sustaining the forgery.

**The clamp bounds one announcement and cannot see a rate.** An attacker
replaying a well-formed announcement every fifth superframe holds a device asleep
indefinitely with every individual beacon inside spec. So both ends also enforce
`RADIO_QUIESCE_MIN_GAP = 4`: a fresh quiesce is refused unless normal traffic ran
for four superframes since the last one ended. That caps quiesce at half the air
time. An operator pairing several devices back to back pays for it in wall clock;
a forger cannot exceed it either.

The hub enforces the same limit on itself. A hub that could exceed what devices
accept would look like an attacker to its own network — and would be
indistinguishable from one, which is worse.

The real fix is authenticating the beacon, which needs a network broadcast key
shared by every paired device. That key has the property such keys always have —
one compromised node can forge broadcasts — and that trade is worth making
deliberately rather than inheriting. It is not built.

Compare [joining.md](joining.md#open-issue-the-join-beacon-is-unauthenticated):
the join beacon is unauthenticated *by necessity* and cannot be fixed. The data
beacon is unauthenticated *because nothing has sealed it yet*, and can be.

## Only the named device can trigger it

`handle_join_frame` accepts a `PAIR_REQ` only when a window is open **and**
`dev_id` matches the one the operator named. Otherwise any frame arriving on a
fixed, published channel during a 60 s window could cost the network 8 seconds.

## The exchange

Four frames, all on the join channel, `pair_v2`.

```
device -> hub  PAIR_REQ     57 B  cleartext  id, nonce, public key
hub -> device  PAIR_RSP     59 B  cleartext  hub ephemeral, confirm_hub
device -> hub  PAIR_CONF    26 B  cleartext  confirm_dev
hub -> device  PAIR_ACCEPT  50 B  sealed     slot, rate, network hop key
```

`Z = X(hub_static · dev_static) || X(hub_eph · dev_static)`. The first term
authenticates the hub — only the holder of the hub's private key can compute it
— and the second supplies the hub's freshness. Keys are `HKDF(Z, salt, info)`
with

```
salt = hub_id(4) || dev_id(4) || req_superframe(4) || dev_nonce(8)
```

### Both ends must contribute freshness

The first version bound nothing fresh into the derivation: `Z` was a function of
the two static keys and `hub_eph` alone. So a recorded `PAIR_RSP`, replayed at
the device's next pairing, re-derives the identical session key, `confirm_hub`
verifies, and the replayed `PAIR_ACCEPT` opens — **the device installs an old
key with every check passing**. The attacker needs no key material at all.

Found by the device side. Two things follow from it and both are now in the
salt and the transcript:

- **`req_superframe`** is monotone and durable through the flash ceiling, so a
  value can never recur.
- **`dev_nonce`**, eight bytes from the device's RNG, because the superframe
  alone is not enough: an unpaired device takes its counter from a **join
  beacon, which is cleartext by necessity**, so a forged beacon feeds it
  whatever superframe the recording used.

**The hub refuses a nonce it has seen from that device before.** That covers a
stuck RNG in every mode, not only the all-zero one — all-zero is the *unseeded*
signature, and an RNG stuck at any other constant walks past a zero check. It
refuses a replayed `PAIR_REQ` as a side effect. The device side found that its
own RNG can reach `PAIR_REQ` having never produced a word since power-on,
because the device's identity is loaded from flash rather than drawn.

### The hop key is a network key

`pair_v1` derived it from `Z`, which is **pairwise**. Every device would compute
a different permutation, and the hub has one radio and sends one beacon per
superframe on one channel — so at most one device in the network could ever hear
it. It works perfectly with a single device on a bench, which is why neither side
saw it; it fails at two, and it fails as "device B hears nothing at all".

The hop key is now 16 random bytes the hub generates once and keeps, delivered
to each device **inside `PAIR_ACCEPT`, sealed under that device's session key**.
Deriving it from `hub_static` instead would need no storage and no delivery, and
does not work: every device is provisioned with the hub's *public* static key,
so anything derived from it is derivable by anyone and the sequence stops being
keyed.

`pair_v1`'s `pair_key_hop` stays pinned and **unused**, and the vector file says
so in as many words — an omission and an oversight look identical to the next
reader.

**There is no rotation path for it, and that is a chosen property.** `PAIR_ACCEPT`
is the only sealed downlink that exists, so changing the hop key means re-pairing
every device. A per-epoch network root does not help: leaking the root leaks
every epoch, so it buys consistency with the session-key ratchet and no security.

### The sealed grant is 19 bytes on purpose

Not a multiple of four. `HAL_CRYP_Decrypt` in GCM mode does not mask the unused
bytes of a partial final word while encrypt does, so such a length fails the tag
with byte-perfect ciphertext — which on air looks like a radio fault. Making the
first frame a device ever opens one of these means the defect cannot wait.

Verified as a *detector* rather than a gesture: filling the GCM input block with
`0xFF` instead of zeroing it fails exactly the decrypt paths whose length is not
a multiple of four, and leaves the 8-byte uplink and every encrypt path green.
The general rule — **when you pick a test length to catch a known defect, check
that a length outside the defect's class still passes** — came from the device
side.

## What is not built

- **Device-side honouring.** The rules above are a contract the device half has
  to implement. Four rules belong to the device alone and are recorded here so
  the hub side does not contradict them:
  - `resume_in` is clamped to `RADIO_QUIESCE_SUPERFRAMES`.
  - A later announcement may bring `resume_at` **forward** but never push it out.
    The hub commits at announce time and never extends, so an announcement naming
    a later resume is a bug or a forgery either way.
  - A quiesce is honoured only if the device was **already aligned before this
    beacon** — not "is aligned now". Alignment succeeds on the beacon being
    parsed, so checking afterwards can never fail. The distinction matters
    because a device that has just booted has no opinion about where the counter
    should be and must take the first one it hears on trust: that first beacon
    carries an unchecked counter, and a forger only needs to reach the device
    between power-on and its first honest beacon.
  - A device that just rejected a beacon ignores the next announcement. See the
    announce-run note above for why that is safe.
- **Beacon authentication**, per the section above.
- **Key rotation.** Only generation 0 exists. The agreed base case is
  `R(0) = Z`, so generation 0 is already `pair_v2`'s pinned session key and no
  vector changes when the ratchet is written; the alternative,
  `R(0) = HKDF(Z, salt, "openhub/v1/root", 64)`, is hygienically better and
  costs a `pair_v3`. The hub deliberately stores **no** ratchet state today
  rather than a value from the scheme that was not chosen.

  **A hash ratchet gives forward secrecy only if the previous state is
  destroyed, and neither store can destroy it.** Both are append-only and never
  erase in service, so `R(n-1)` stays readable until the sector swaps — which
  has nothing to do with the daily rotation. The ratchet still bounds what a
  live key compromise gives an attacker and still costs no air. It is the
  sentence "keys rotate daily, so old keys are unrecoverable" that is false, and
  that is the sentence everybody writes.
- **A downlink queue.** `devices rate <n>` changes what the *next* pairing is
  granted; there is no way to retune a device already in the field.

## Verification

All on air, with the [SDR bench](../testing/sdr.md).

**The quiesce lasts exactly as long as announced.** `device quiesce 4` fired
repeatedly across a 45 s wideband capture, hub bursts extracted by air time and
cadence:

| Announce → resume | Superframes |
|---|---|
| 7999.1 ms | 4.00 |
| 8002.6 ms | 4.00 |
| 8001.7 ms | 4.00 |

against a 2001.1 ms superframe. A baseline capture with no quiesce showed gaps of
1 or 2 superframes only and never 4 — the 2s are beacons that fell on one of the
three hop channels outside the 2.4 MHz capture. Never longer than announced,
which is the property devices sleep against.

**The join region is where the grid says it is.** Wideband capture with a window
open, offset from each data beacon to the join beacon that follows it:

```
nominal grid offset 1874.0 ms
measured 1873.9 .. 1874.8 ms over 5 frames (mean 1874.4)
```

Join beacons appeared every second superframe, and the data beacon train stayed
on its 2001 ms cadence throughout.

**The state machine is checked without the radio at all.** The hub counts what it
put on air, and the counts have to close:

```
device pair
beacons 17 (6 announce), 4 silent, 12 refused
```

`data_beacons + silent_frames` must equal the superframes elapsed, `announce`
must be `RADIO_QUIESCE_ANNOUNCE` per quiesce, and hammering `device quiesce` must be
refused. Measured over 21 superframes with one request per second: 17 + 4 = 21
exactly, 6 announcements for 3 accepted quiesces, 12 refusals. This is the check
that does not care whether the spectrum is clean, and it is the one that caught
the announce-run and rate-limit behaviour precisely.

**A trap worth recording.** Three narrowband captures on 866.5 MHz found *zero*
join beacons while the firmware counted 48 transmitted with no errors — which
reads exactly like a broken feature. Two causes, both in the bench rather than
the firmware:

- **`capture.py`'s defaults clipped an FSK tone.** The signal sits 100 kHz above
  the capture centre and the default 250 kS/s puts the band edge at 125 kHz, so
  with ±25 kHz deviation the upper tone landed exactly on Nyquist. One tone
  survives and the discriminator produces nothing. `capture.py` now warns.
- **The analysis script rotated the wanted channel to +100 kHz and then
  low-passed at 96 kHz**, deleting it. Caught only by running a control —
  pushing a frame *known* to decode through the same path, which also produced
  "sync not found". Without that control the hunt would have moved into the
  firmware.

`tools/sdr/pluck.py` came out of it: cut one channel and one moment out of a
wideband capture and re-emit it as a normal `.iq`, so the wanted channel is the
only thing in the file. `decode.py` thresholds against the peak of the whole
capture, so on a wideband recording of a hopping transmitter the loudest thing in
the band decides what counts as a burst.

**Resolved, after two more causes.** The narrowband captures were failing for
two further reasons, neither in the firmware:

- **`-s 500e3` is a rate the RTL2832U cannot produce.** It supports 225–300 kS/s
  and 900 kS/s–3.2 MS/s and nothing between. `rtl_sdr` warns and captures anyway
  at whatever rate the hardware was last programmed with — possibly by another
  session — and `capture.py` wrote the *requested* rate into the `.meta`, so
  every duration downstream was scaled by an unknown factor. `capture.py` now
  refuses out-of-range rates and treats the warning as fatal.
- **`find_bursts` thresholded at a fraction of the peak alone.** The hub's signal
  reaches this dongle only ~12 dB above the noise, so a quarter of the peak sat
  *below* the noise floor, half the samples read as "on", and every burst merged
  into one. The threshold is now the higher of a peak fraction and a multiple of
  the noise floor.

With both fixed, the join beacon decodes byte for byte — see
[joining.md](joining.md#verification). The lesson that generalises: **a relative-
to-peak threshold degenerates when the peak is close to the floor**, and the
symptom is indistinguishable from a transmitter that never keyed.

## See also

- [tdma.md](tdma.md) — the slot grid the join region sits in
- [joining.md](joining.md) — the join channel and why it is fixed
- [ADR-0020](../decisions/0020-device-triggered-quiesce.md)
- [security/key-lifecycle.md](../security/key-lifecycle.md) — what the exchange will do
