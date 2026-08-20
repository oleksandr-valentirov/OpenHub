# Frequency hopping

**Status: implemented, unit-tested, visible on the waterfall.**

`Common/src/hop.c` decides which channel a superframe uses. It is a **keyed
shuffle indexed by the superframe counter** — not a counter walked upwards, and
not an LFSR stepped once per frame.

```c
uint8_t hop_channel(hop_ctx_t *ctx, uint32_t superframe);
```

## The three properties that forced the design

### Both ends must agree, so the secret comes from pairing

The channel has to be a deterministic function of *shared* state. The RNG belongs
at **pairing**, where its output becomes a secret both ends hold. A value drawn
per hop would be one only the hub knows, and the device could not follow it.

This is the correction to an intuitive but unworkable idea: "use the RNG to pick
the channel." The RNG picks the *key*; the key and the counter pick the channel.

### A sleeping node must catch up in O(1), so the generator is stateless

Indexing by the counter — rather than stepping a generator — means a node that
slept through a thousand superframes reads the counter from a beacon and computes
the current channel directly. Nothing to fast-forward, nothing to resync.

An LFSR stepped once per frame fails exactly here: after a long sleep the node
must iterate it a thousand times, and if it ever loses count it is silently on the
wrong channel with no way to notice.

### Occupancy must be uniform, so it is a permutation and not a modulo

Taking a PRF or an LFSR output modulo 29 does **not** give a flat channel
distribution. Measured over 1160 frames:

| Approach | Occupancy per channel (ideal: 40) | Immediate repeats |
|---|---|---|
| PRF mod 29 | 26 … 54 | 3% |
| Fisher-Yates per cycle | 40 exactly | **0.11%** |

**Not zero, and the difference matters for how the test is written.** Fisher-Yates
guarantees no repeat *within* a cycle and says nothing across one: the last
channel of a deck equals the first of the next with probability 1/count. Measured
over 400 cycles across five keys: 0.107–0.134%.

Preventing it would cost the statelessness — you would have to keep the previous
deck — which is not worth 0.11%. The argument against PRF-mod-N holds by a factor
of about 30 rather than absolutely.

A repeat means two consecutive superframes on the same channel, which is precisely
what hopping exists to avoid: a narrowband interferer that kills one frame kills
the next as well.

So each cycle is a **Fisher-Yates permutation** of the channel set, and
`hop_channel` returns `deck[superframe % count]`. Every channel is used exactly
once per cycle, occupancy is flat by construction rather than in expectation, and
the revisit interval is bounded.

## Why not a linear sequence

"Channel + 1 each frame" is the cheapest thing that technically hops. It is also
the worst option available: **96.6% of hops land within 200 kHz of the previous
one**, so a single interferer follows the signal trivially, and the sequence is
100% predictable to anyone who watches two frames.

It is also the version that looks least like hopping on a waterfall — a diagonal
line rather than a scatter.

## The PRF

One AES-128 block, run **once per cycle** — 28 superframes, about 56 seconds —
seeded by the cycle number. Two blocks give enough material to shuffle the deck.

```c
typedef int (*hop_prf_t)(void *ctx, const uint8_t in[16], uint8_t out[16]);
int hop_channel(hop_ctx_t *ctx, uint32_t superframe, uint8_t *channel);
```

**The PRF must be able to fail, and the caller must be able to hear it.** Both
originally returned nothing useful, and the consequence is the worst shape
available: if the PRF fails, the shuffle runs over an uninitialised buffer and
**still produces a perfectly valid permutation**. Occupancy stays flat, no
repeats appear, nothing looks wrong — and the hub hops on a sequence no device
can compute. A failure that produces plausible output and no error.

The deck is also invalidated *before* rebuilding, so one half-built by a failing
PRF can never be served as though it were cached. On a failure `radio.c` stays
silent rather than transmitting on a guessed channel.

This is not hypothetical: `HAL_CRYP_SetConfig` failing is exactly what contention
with the per-frame GCM cipher will look like.

The PRF is injected, so `hop.c` has no platform dependency and runs in host tests.
On CM4 it is `hop_prf_aes`, which drives CRYP in ECB mode. Reconfiguring CRYP per
call is wasteful, but at once per minute the cost is irrelevant and the
accelerator is otherwise idle.

**It must set `CRYP_DATATYPE_8B` explicitly.** The `.ioc` configures the handle
for `CRYP_DATATYPE_32B`, and with a 32-bit datatype the accelerator consumes the
buffer word-wise — so on this little-endian core the block actually encrypted is
the input with every group of four bytes reversed, and the output comes back
reversed the same way. A perfectly good PRF, and not the function anyone reading
the code would write down.

Measured rather than argued, with the `hopprf` console command against a host
AES: with the 32-bit datatype the hub produced `7d2a9f80…`, which matches AES
over the *word-swapped* block; with 8-bit it produces `7aca0fd9…`, which matches
AES over the bytes.

**The cycle counter goes into the block big-endian**, by the same rule that
governs every other crypto input. It was little-endian, and the reason that
survived is instructive: **cycle 0 is byte-identical under both conventions**, so
any test inside the first cycle — the first 56 seconds of a hub's life — passes
and everything after it diverges.

**A caution for the future:** `hop_prf_aes` does a
`HAL_CRYP_GetConfig` / `SetConfig` dance to switch CRYP into ECB. Once per-frame
AES-GCM also runs on CRYP, that shared configuration becomes a real hazard, and
the two users need to be serialised deliberately rather than by luck of timing.

## Tests

```bash
make -C Common/test check
```

Covers: every cycle is a permutation; a jump of N superframes gives the same
answer as stepping N times; a different key gives a different sequence; zero
immediate repeats; argument validation.

## Interaction with joining

`hop.c` yields indices `0 … 27`. `radio.c` maps them onto the 29-slot grid with
`hop_slot_to_grid`, which skips the reserved join slot:

```c
return (hop_index < RADIO_JOIN_SLOT) ? hop_index : (uint32_t)hop_index + 1u;
```

So the hopping set and the join channel are disjoint **by construction**, not by
probability. See [joining.md](joining.md).

## Seeing it

`tools/sdr/hops.py` detects bursts on a spectrogram and prints the channel
sequence, so the scatter can be checked against the computed deck rather than
eyeballed. See [testing/sdr.md](../testing/sdr.md).

## See also

- [ADR-0008](../decisions/0008-keyed-shuffle-hopping.md)
- [tdma.md](tdma.md) — where the superframe counter comes from
- [security/key-lifecycle.md](../security/key-lifecycle.md) — where the hop key comes from
