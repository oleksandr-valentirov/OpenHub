# ADR-0008 — Hop by keyed shuffle indexed by the superframe counter

**Status:** Accepted
**Date:** 2026-08-20

## Context

Frequency hopping needs a rule for choosing a channel that both ends compute
identically. The starting proposal was to combine a polynomial generator, the
hardware RNG and the timer.

Each part needed separating:

- **The RNG** cannot be sampled per hop — a value only the hub knows is a value the
  device cannot follow. Its correct place is **pairing**, where its output becomes a
  secret both ends hold.
- **The timer** determines *when* a hop happens, not *where* it goes.
- **A polynomial (LFSR)** works as a generator but is *stateful*, and battery
  devices sleep for long periods.

Three candidate rules were measured over 1160 frames on 29 channels (ideal
occupancy: 40 per channel):

| Rule | Occupancy | Immediate repeats | Predictable |
|---|---|---|---|
| Channel + 1 | flat | 0 | **100%** — and 96.6% of hops within 200 kHz |
| PRF mod 29 | **26 … 54** | **3%** | no |
| Fisher-Yates per cycle | **40 exactly** | **0.11%** | no |

(The shuffle figure was recorded as 0 and is not: no repeat is guaranteed
*within* a cycle, and across a cycle boundary one occurs with probability
1/count. Measured over 400 cycles: 0.107–0.134%. The argument holds by a factor
of ~30 rather than absolutely.)

## Decision

`Common/src/hop.c` computes a **Fisher-Yates permutation of the channel set per
cycle**, seeded by a keyed PRF over the cycle number, and returns
`deck[superframe % count]`.

The PRF is one AES-128 block, injected as a callback, run once per cycle (28
superframes, ~56 s). The key is a **network** key: 16 bytes the hub generates
once and delivers to each device inside `PAIR_ACCEPT`, sealed under that
device's session key.

**It cannot come from the pairing secret**, which this ADR originally said it
did. That secret is pairwise, so every device would compute a different
permutation while the hub has one radio and sends one beacon per superframe on
one channel - at most one device could ever hear it. Correct with one device on
a bench and broken at two, presenting as "the second device hears nothing".
See [radio/pairing.md](../radio/pairing.md).

## Consequences

- **Occupancy is flat by construction**, not in expectation. Every channel is used
  exactly once per cycle and the revisit interval is bounded.
- **Zero immediate repeats.** A narrowband interferer that kills one frame cannot
  kill the next — which is the entire point of hopping.
- **Stateless across sleep.** A node that slept a thousand superframes reads the
  counter from a beacon and computes the current channel directly. Nothing to
  fast-forward, nothing to resync, and no way to silently lose count.
- The PRF cost is one AES block per minute — negligible, and the accelerator is
  otherwise idle.
- `hop.c` has no platform dependency, so the properties above are unit-tested on a
  host rather than argued for. The host test asserted **zero** repeats, which is
  not a structural property: over its 600 hops the expectation is 0.7, so it
  passed roughly half the time by luck and would eventually have failed looking
  like a regression. It now bounds the rate instead.
- **A caution:** `hop_prf_aes` borrows CRYP by saving and restoring its
  configuration. Once per-frame AES-GCM also uses CRYP, the two users must be
  serialised deliberately.

## Alternatives rejected

**Linear (channel + 1).** Cheapest, and worst: fully predictable from two observed
frames, and 96.6% of hops land within 200 kHz of the previous one, so one
interferer follows the signal trivially.

**PRF or LFSR modulo N.** Modulo bias gives occupancy between 26 and 54 where 40 is
uniform, and 3% of hops repeat the previous channel.

**LFSR stepped per frame.** Stateful. After a long sleep the node must iterate it
thousands of times, and losing count puts it silently on the wrong channel.

**RNG per hop.** Unfollowable by the device. This was the misconception worth
correcting explicitly: the RNG picks the *key*, and the key plus the counter pick
the channel.

## See also

[radio/hopping.md](../radio/hopping.md)
