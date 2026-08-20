# ADR-0020: the pairing quiesce is device-triggered, bounded and announced with a resume time

**Status: accepted.** Schedule implemented and measured on air; the key exchange
inside it is not built.

## Context

The hub has one radio and pairing happens on a different channel from data. The
exchange needs the hub to *listen* on the join channel for extended periods, with
compute pauses of tens of milliseconds on both sides, so it genuinely conflicts
with serving the grid.

The starting proposal was a pairing mode: broadcast a notice to all devices, stop
hopping and TDMA, pair, restart.

## Decision

Three changes to that shape.

**1. The suspension is triggered by the device, not the operator.** `device add`
opens a 60 s window during which the grid keeps running and the hub adds a join
region to the tail of each superframe. The grid is suspended only when a valid
`PAIR_REQ` actually arrives, for 4 superframes.

The window and the exchange have different durations by two orders of magnitude:
60 s is a human pressing a button, 8 s is the protocol. Suspending for the human
part costs the network 60 s of service every time an operator opens a window,
including the times the device never appears.

**2. The announcement carries a resume superframe, not a flag.** `resume_in` in
the data beacon, so `resume_at = superframe + resume_in`.

"Wait for the next broadcast" cannot be the rule, because the announcement is the
last frame a device will hear — the hub is leaving. A device told only
"quiescing" must stay in receive to discover when it ended, which is the opposite
of standing down, and costs 8 s of receiver current on 64 nodes to learn
something the hub already knew when it spoke.

**The resume superframe is never extended.** Devices sleep against it while
nothing is listening; moving it strands all of them. An exchange that overruns
loses its window.

**3. The superframe counter does not stop.** A quiesce suspends transmission, not
time.

## Consequences

**A quiesce needs no resynchronisation.** The hop sequence is indexed by the
counter rather than stepped by it ([ADR-0008](0008-keyed-shuffle-hopping.md)), so
a device that slept through the silence computes the channel for the superframe
it wakes into and is back. The stateless hop design was chosen for sleeping
devices and pays for this for free.

**Freezing the counter would have been nonce reuse.** Repeated superframe numbers
repeat GCM nonces once frames are sealed — the same defect already fixed once,
when the counter advanced only on a successful transmit.

**The join region costs nothing when nobody is pairing.** It overlays the uplink
tail past slot 95 rather than being reserved, and slots are assigned from 0
upward.

**The announcement is sent twice, and that constant is load-bearing for two
independent reasons.** One copy is one lost frame away from a device that wakes
into its slot and transmits at a hub that is not listening. Separately, the
device rejects an announcement arriving on the first beacon after a rejected one,
so a corrupt or forged frame immediately before an announce run consumes the
first copy — with one copy that silently costs a legitimate quiesce. Anyone
tuning `RADIO_QUIESCE_ANNOUNCE` back to 1 to save 8 ms of air will not find the
second reason by reading either side alone.

**The per-beacon clamp bounds one announcement and cannot see a rate**, so both
ends also enforce `RADIO_QUIESCE_MIN_GAP`: four superframes of normal traffic
between quiesce periods. Without it, an attacker replaying a well-formed
announcement every fifth superframe holds a device asleep indefinitely with every
individual beacon inside spec. The hub applies the limit to itself — a hub that
could exceed what devices accept would be indistinguishable from an attacker.

**The quiesce is a denial-of-service primitive until the data beacon is
authenticated.** Bounded by three things, none cryptographic: a forger must hold
the hop key to be on the right channel at the right superframe; both ends clamp
`resume_in` to 4; and standing down means not transmitting, not going deaf. The
real fix is a network broadcast key, with the property such keys always have —
one compromised node forges broadcasts — and that trade should be made
deliberately.

**The data beacon grows to 14 bytes and version 2.** Its header said the layout
was frozen and that growth changes the version; it did.

## Alternatives

**Suspend for the whole 60 s window.** Simpler, and the reason it was not chosen
is availability: it spends the network's uptime on a human's reaction time.

**Never suspend; pair inside a per-superframe join region only.** Attractive —
the network never stops. Rejected because the exchange has compute pauses on both
sides that do not fit a 100 ms region, and stretching it across many superframes
multiplies the chances of a joiner losing alignment mid-exchange. The region does
the *listening*; the quiesce does the *exchange*.

**A dedicated announcement frame instead of beacon fields.** Costs duty cycle
every superframe to say nothing almost every time. Two bytes in a frame that
already goes out is free.

## Verified

`device quiesce 4` cycled across a 45 s wideband capture: announce-to-resume gaps of
7999.1, 8002.6 and 8001.7 ms against a 2001.1 ms superframe — exactly 4
superframes, three times, never longer. A baseline capture showed gaps of 1 or 2
superframes only.

Join region measured at 1873.9–1874.8 ms after the data beacon against a nominal
1874.0 ms, with beacon jitter unchanged at 1–4 µs.

The state machine is checked independently of the radio by the hub's own
transmission counters, which must close: over 21 superframes with a quiesce
requested every second, 17 beacons + 4 silent superframes = 21 exactly, 6
announcements for 3 accepted quiesces, and 12 requests refused by the rate limit.

See [radio/pairing.md](../radio/pairing.md).
