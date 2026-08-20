# ADR-0021: pair_v3 changes discovery, not the key exchange

**Status: accepted, not implemented.** Agreed with the device side on
2026-08-20; their answers are folded in below and changed two things. Frame
sizes and duty-cycle arithmetic are settled. `PAIR_INIT` is pinned in
`Common/test/vectors/pair_v3.txt`, which **has no consumer yet** - it reproduces
`pair_v2`'s published `pair_z1`, and that is all it currently proves.

## Context

In `pair_v2` the device speaks first. An unpaired device hears a cleartext join
beacon, decides a hub is present, and transmits `PAIR_REQ` carrying its public
key. Three consequences, none of them fatal and all of them avoidable:

- **The device transmits blind.** It has no evidence the beacon came from its
  hub, because the join beacon is cleartext by necessity - it is the one frame a
  device can read before it has a key. A forged beacon makes a battery node key
  up and spend a P-256 scalar multiplication on nobody.
- **The device retries into silence.** It cannot know whether the hub heard it,
  so the retry schedule is guesswork on the side with the smaller battery.
- **`PAIR_REQ` carries a 33-byte public key** the hub could already have known,
  making it the second-largest frame in the system at 57 bytes.

The operator already performs an out-of-band step: they type a value the device
printed. Nothing about that step requires the value to be a *hash*.

## Decision

**The hub initiates.** An unpaired device only listens on the join channel. The
hub, while an operator's window is open, transmits an addressed and
authenticated `PAIR_INIT` naming the device it wants. The device answers, and
from that point the exchange is `pair_v2` unchanged.

That last clause is the point of the design. The KDF, the transcript, the two
confirmations, the sealed grant and the `pair_v2` vectors do not move. Two
independent implementations agree on those bytes today; the change is which
side speaks first and how the conversation starts, not what is said.

### The operator enrols a public key, not a fingerprint

The device side confirmed it printed only the fingerprint and now prints both,
with the key **above** it: one is the enrolment value and the other is a check on
it, and adjacency is what stops them being typed into the wrong command. The
33 bytes were verified byte-identical to what `PAIR_REQ` put on the wire, and
host Python over them reproduces the fingerprint - key, fingerprint and frame
checked as one set from outside both firmwares.

Both ends can compute `Z1 = X(hub_static · dev_static)` before any frame exists,
and the hub's first frame can therefore be MACed under a key derived from it.
That is what makes an addressed first frame safe to act on.

The hub cannot do this today: `ks_record_t` stores `fingerprint[32]`, which is
SHA-256 of the device's compressed point, and a hash cannot be turned back into
a curve point. So enrolment takes the 33-byte compressed public key itself.

The security property is unchanged. The fingerprint's job is to authenticate the
device's key over an out-of-band channel; a public key carried over the same
channel is self-authenticating and strictly more informative. The fingerprint
remains what `device list` displays - computed from the stored key rather than
stored beside it, so the two can never disagree.

**The record stays 128 bytes.** `fingerprint[32]` becomes `pubkey[33]` and
`spare[4]` becomes `spare[3]`. No change to the flash-word alignment, the sector
layout or the capacity.

**Existing enrolments cannot be migrated**, and that is the one-wayness working
as designed rather than a defect in the migration. The store is append-only,
version-gated and never erases; old records are stepped over, exactly the path
that already exists. Every enrolled device is re-enrolled by the operator typing
the longer value.

### PAIR_INIT cannot carry the ephemeral point

This is the constraint that fixed the frame layout, and it is invisible from the
layout itself.

`PAIR_INIT` is **retried across the 60 s window**, so it is recurring hub air,
competing with the beacon and the downlink inside the same 1%. Every other
pairing frame is sent once, inside a quiesce, where the hub has stopped its own
traffic - which is why `PAIR_RSP` may be 22.4 ms and this frame may not.

Measured against the real constants (25 kbps, 11 bytes of PHY overhead, 2 s
superframe, beacon 0.400% + downlink 0.336% already spent):

| PAIR_INIT | every 2 sf | every 4 sf | every 8 sf |
|---|---|---|---|
| **28 B** (no ephemeral) | 1.048% over | **0.892% ok** | 0.814% ok |
| **61 B** (with ephemeral) | 1.312% over | 1.024% over | 0.880% ok |

At 61 bytes the frame is over budget at every retry rate fast enough to be
useful, and the only rate that fits gives 3 attempts in a 60 s window. So the
ephemeral point stays where `pair_v2` already puts it, in `PAIR_RSP`.

At 28 bytes every 4th superframe the frame costs 0.156%, which is **less than
the 0.200% join beacon it replaces**. Discovery gets cheaper, not dearer.

### The frames

| # | Frame | Direction | Bytes | Change |
|---|---|---|---|---|
| 1 | `PAIR_INIT` | hub -> dev | 28 | new; replaces the join beacon during a window |
| 2 | `PAIR_REQ` | dev -> hub | 24 | `pubkey[33]` removed - the hub already has it |
| 3 | `PAIR_RSP` | hub -> dev | 59 | unchanged |
| 4 | `PAIR_CONF` | dev -> hub | 26 | unchanged |
| 5 | `PAIR_ACCEPT` | hub -> dev | 50 | unchanged |

`PAIR_INIT` is `type, version, net_id, hub_id, dev_id, superframe, mac[12]`.
The MAC is HMAC-SHA256 under `HKDF(Z1, "pair_v3 init")`, truncated to 96 bits:
the frame has 37 bytes of headroom under the FIFO ceiling and spending 4 of them
on tag length buys nothing against a forger who gets one attempt per retry.

`PAIR_REQ` keeps `dev_nonce[8]`. The device contributes no ephemeral key in
`pair_v2` and still does not here, so the nonce remains its only freshness -
removing it would restore precisely the replay `pair_v2` was written to close.

**`Z1` is static per pair and cached, not recomputed.** One scalar
multiplication per device, ever, on each side. The per-pairing cost is unchanged
from `pair_v2` on both ends.

## Consequences

- The device never transmits unsolicited, and never transmits at all to a hub
  that has not first proved it holds `dev_static`.
- Device air for the exchange drops from 21.76 ms to 11.2 ms for the request.
- A join channel with a window open carries nothing an unenrolled listener can
  act on, where today it carries a broadcast invitation.
- The hub must hold a curve point per device rather than a hash, so a keystore
  read now yields material an attacker can use to *impersonate the hub to that
  device* only in combination with the hub's own private key - unchanged in
  substance, but the record is no longer free of the device's key material.
- **The 65-byte ceiling is now load-bearing.** `radio_phy.h` carries
  `RADIO_MAX_PAYLOAD_B` and `radio_protocol.h` asserts every frame against it.
  Nothing may grow past it without a second frame.

## The broadcast join beacon is removed

Left open above and settled by the device side, which owns what a reset device
does. A factory reset there draws a **new `dev_id` and a new keypair**, so:

- **Identity survives, session lost** (reflash, power cut): the hub still holds
  the key, `PAIR_INIT` addresses the device, no broadcast is needed. This is the
  common case and already works - pairing survived a reset and a reflash.
- **Identity destroyed:** the hub's record is stale by construction. Its
  `pubkey[33]` names a keypair that no longer exists, so no MAC verifies and a
  broadcast beacon does not help either. The operator must enrol the new key.

There is no third case, and `PAIR_INIT` only goes out while an operator has a
window open - so the hub always knows which device it is inviting. The
broadcast's only remaining job would be announcing the hub to a device that
cannot act on the announcement.

**This frees no duty cycle**, and the tempting inference that it does is worth
recording. The join beacon transmits *only while a window is open*, which is
exactly when `PAIR_INIT` replaces it: the two never coexist, so the table above
was never spending the 0.200% in the first place. Retry stays at every 4th
superframe. Two quantities that are each individually correct - the beacon's
cost and the budget's headroom - do not compose here because they belong to
different windows.

## PAIR_INIT is replayable, and the mitigation is device-side

Raised by the device side and not fixable from here. **A device that needs
pairing has no counter**: it has not followed a beacon, which is the state it is
in. So it cannot check `superframe` against anything, and a recorded
`PAIR_INIT` replays forever regardless of MAC length.

The cost is bounded and does not change the design. The device answers with a
fresh `dev_nonce`, the attacker cannot produce `PAIR_RSP`, and the exchange
dies. A replay buys **one device transmission** - battery drain and wasted air,
not key compromise.

**No new field.** A monotonic invitation counter was proposed and `superframe`
already is one: the hub's flash ceiling guarantees it never repeats or regresses
across a reset. A second counter would be a value that can disagree with it. The
missing piece is not a field but persistence on the device side - the high-water
`superframe` at which a `PAIR_INIT` was accepted.

**Device-side obligation, recorded because the hub cannot observe it:** rate-limit
responses to `PAIR_INIT` and stop answering after repeated failed exchanges. From
the hub an unrated device is indistinguishable from a working one until a battery
is flat.
