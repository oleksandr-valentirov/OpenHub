# Key lifecycle

**Status: proposed.** Nothing here is implemented.

## The keys

| Key | Lifetime | Held by | Purpose |
|---|---|---|---|
| Device long-term P-256 keypair | device lifetime | the device | authenticates it to the hub |
| Hub long-term P-256 keypair | hub lifetime | the hub | **generated but unused — see below** |
| Pairing secret | one pairing | both | ECDH output, immediately consumed |
| Session key (AES-128) | one day | both | frame encryption |
| Hop key | one day | both | drives [the hop sequence](../radio/hopping.md) |

The session key and the hop key are **separate keys** derived from the same secret
with different HKDF `info` strings. One key used for two purposes is how protocols
acquire subtle flaws.

## Provisioning

### The authentication is one-way today, and the hub is the unauthenticated end

**Stated plainly because the table above used to claim otherwise.** The device is
authenticated to the hub by the fingerprint the operator supplies. There is no
equivalent in the other direction: both confirmations are HMACs under keys
derived from the ECDH secret, and **ECDH is anonymous**. Any attacker who
completes an exchange with a device derives a valid secret, computes a correct
`hub_confirm`, and the device cannot tell it from the real hub. `hub_confirm`
proves liveness and agreement on the transcript — that the peer computed the same
secret — and proves nothing about identity.

The hub's long-term keypair does not fix this by existing. Nothing in the
exchange uses it.

Found by the device side reading the draft exchange spec, and it is the mirror of
the enrolment correction: "the CLI is the missing piece for authentication"
became "enrolment is a precondition for authentication", and this is the same
sentence pointing the other way.

#### What the options cost

| | hub authenticated | fresh secret | forward secrecy | extra wire bytes |
|---|---|---|---|---|
| today: `ECDH(dev_static, hub_ephemeral)` | **no** | yes | **no** | 0 |
| both keys static | yes | **no** | no | 0 |
| hub signs the transcript | yes | yes | no | **+64, breaks one FIFO load** |
| two ECDH, hub static provisioned | yes | yes | **no** | **0** |
| the above plus a device ephemeral | yes | yes | **yes** | **+33, breaks one FIFO load** |

**The forward-secrecy column is the one that was nearly lost silently**, and it
reframes the decision: today's design does not have forward secrecy either.
Every term in both today's construction and the recommended one uses
`dev_static`, so an attacker who records an exchange and later extracts a
device's long-term private key recomputes the secret — `hub_static` is public
and `hub_ephemeral`'s public half was on the wire.

So the recommended option **does not give up forward secrecy; it never existed
here.** It is strictly better than today: same properties, plus hub
authentication, for no wire bytes.

The asymmetry is worth stating because it runs the unhelpful way. Compromising
the **hub's** static key does not break past sessions — the second term still
needs an ephemeral private that was discarded. Compromising a **device's** does.
And devices are the units left unattended in the field, while the threat model's
"physical possession is game over" line was written about the hub.

The counter, and the reason this is accepted rather than fixed: an attacker
holding a device already has its current session key, every future key the
ratchet derives, and the ability to impersonate it. Reading that one device's
past sensor traffic is a marginal addition to a compromise that is already
total for that node.

The signature option is not merely expensive: a 64-byte signature pushes
`PAIR_RSP` past the 66-byte FIFO, and fitting one load is the constraint that
drove [ADR-0018](../decisions/0018-compressed-points-on-the-wire.md).

**The last row is the recommendation.** Provision the device with the hub's
static *public key* out of band — symmetric with the hub being provisioned with
the device's fingerprint — and derive

```
secret = HKDF( ECDH(dev_static, hub_static) || ECDH(dev_static, hub_ephemeral) )
```

The first term authenticates the hub, because computing it needs the hub's
long-term private key. The second supplies freshness. The hub's static key is
provisioned rather than transmitted, so **it costs nothing on the wire** and
`PAIR_RSP` stays inside one FIFO load. The cost is one extra scalar
multiplication per side: the hub goes from a measured 330 ms to roughly 500 ms
and the device from ~205 ms to ~310 ms, both far inside the 4 s of clear air a
[quiesce](../radio/pairing.md) provides.

It also settles a related question: whichever hub key is *not* in the transcript
is not bound to the exchange, so both belong there.

#### Adding forward secrecy, if it is ever wanted

It needs a device ephemeral, `ECDH(dev_eph, hub_eph)` as a third term. The
device's static key must still be transmitted — the hub holds only a fingerprint
and needs the key itself to check it — so `PAIR_REQ` carries two points and goes
**49 → 82 bytes against a 66-byte FIFO load**. That is the constraint
[ADR-0018](../decisions/0018-compressed-points-on-the-wire.md) exists to respect,
so it cannot simply be paid.

The way out is a four-frame exchange that splits the two points across frames:
static key first, ephemeral with the device's confirmation later. Every frame
stays inside one load, air time goes from 65.9 ms to about 76.5 ms — still under
the 80 ms a [quiesce](../radio/pairing.md) allows — and the cost is one more
round trip and a longer state machine.

**Not decided, and not to be implemented from this table.** It is recorded so
that the gap is visible and the options are costed, not so that the first person
to read it starts building.

ECDH on its own is anonymous, which means it is trivially man-in-the-middleable:
an attacker who relays the exchange gets a working session with each side.

Authentication comes from **the hub knowing the device's public key before the
exchange begins**, out of band. In practice: the device's identifier and a
fingerprint of its public key are printed on it or shipped with it, and the
operator supplies both to the hub when opening a pairing window.

`device add <id> <fingerprint>` **now takes and persists the fingerprint**, and
`device list` shows it. That is the operator's half of the out-of-band step, and
it survives a reboot — see [architecture/keystore.md](../architecture/keystore.md).

**The exchange checks it now.** `pairing.c` on CM7 hashes the 33-byte compressed
point out of `PAIR_REQ` and compares it against the stored fingerprint in
constant time; a mismatch refuses the exchange before any curve work happens.

The **domain** is the part that had to be pinned rather than assumed: SHA-256 of
the *compressed* SEC1 point, not `0x04||X||Y` and not bare `X`. Three plausible
readings of "SHA-256 of the device public key", three different digests. The
device side was hashing the uncompressed point and truncating to six bytes - the
length would have been caught by the hub's parser, the domain would not. A
32-byte hash of the wrong point **enrols cleanly** and then fails authentication
forever, with the operator having typed exactly what the device printed:
indistinguishable from the attack the check exists to detect.

It is pinned in `Common/test/vectors/pair_v2.txt` as `pair_fingerprint` and
checked by both firmwares' self-tests. `pair_v1` had dropped it, which left the
one value that had already cost a divergence pinned only in an unpublished
candidate.

This is why a forged [join beacon](../radio/joining.md) is tolerable: it can start
an exchange but cannot finish one, because it cannot produce a signature under a
key the operator vouched for.

## Pairing

1. Operator runs `device add <id> <fingerprint>`; the hub opens a 60 s window.
2. The hub beacons on the join channel every second superframe.
3. The device hears the beacon, learns `net_id`, `hub_id` and the superframe counter.
4. Both sides do ECDH P-256, **twice**: a static-static term that authenticates
   the hub and a static-ephemeral term for freshness. The device's public key
   must match the fingerprint the operator supplied, or the hub aborts.
5. HKDF over the two terms yields the session key and both confirmation keys.
   **Not the hop key** - that is a network key the hub already holds, delivered
   sealed in `PAIR_ACCEPT`. Deriving it here gives every device a different
   channel sequence; see [ADR-0008](../decisions/0008-keyed-shuffle-hopping.md).
6. CM7 installs both into CM4 through the [IPC mailbox](../architecture/ipc.md).
7. The device can now compute the hop sequence and is already time-aligned, having
   taken the counter from the beacon in step 3.

**Step 6 runs over a sequence-numbered ring in both directions.** A `PAIR_REQ`
arriving is an event the *radio* core originates, so CM4 sends and CM7 answers -
polling from CM7 instead would make the mailbox work continuously to represent
something that happens when a human presses a button.

The ECDH runs on CM7 — it is far too slow for a radio slot. See
[ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md).

## Daily rotation

Once per day, both ends advance a **ratchet**:

```
  K(n+1) = HKDF(K(n), info = "openhub rotate")
```

No radio exchange, so rotation costs no air time — which matters inside a 1% duty
cycle. Both ends derive independently from a shared schedule.

**Not built.** Only generation 0 exists. Written down because the record already
stores `rotate_epoch` for it.

This *would* give **backward secrecy**: an attacker who recovers today's key
cannot derive yesterday's, because the KDF does not run backwards - **but only
if the previous state is destroyed, and neither store destroys it.** Both the
hub's keystore and the device's are append-only and never erase in service, so
`R(n-1)` stays readable in an older record until the sector swaps. That is a
store-design property, not a KDF one, and an append-only log cannot fix it.

The ratchet still bounds what a live compromise gives an attacker and still
costs no air time. What is false is the sentence everyone writes about it -
"keys rotate daily, so old keys are unrecoverable" - which is a claim broader
than its coverage in exactly the place the next person stops looking. It does **not**
protect against future compromise — today's key derives every future key.
Protection in both directions needs a fresh ECDH, done periodically (monthly, at
a device's own initiative) rather than daily.

### Deletion is confirmation-driven

The rule that makes rotation safe, and the one an implementer is most likely to
get wrong:

> A device keeps `K(n)` until a frame has authenticated under `K(n+1)`, and the
> hub accepts the previous generation during the changeover.

Deleting on schedule instead turns a forged frame into permanent de-pairing: the
counter that drives the schedule reaches a resyncing device in the **cleartext,
unauthenticated join beacon**, so one transmission could make every device in
range delete a key it can never recover. Recovery would need a physical re-pair.

Hence also: the rotation index comes **only from an authenticated frame**; the
fast-forward is **bounded** to one epoch, beyond which a device re-pairs rather
than destroying its key; and the hub's retention of the old key is bounded too,
or a device that never confirms keeps it alive forever.

Full reasoning in [ADR-0017](../decisions/0017-rotation-deletion-is-confirmation-driven.md).

### The tension with stateless hopping, named

[ADR-0008](../decisions/0008-keyed-shuffle-hopping.md) made the hop sequence
stateless on purpose. The ratchet is the opposite — `K(n)` cannot be computed
without walking to it. That is a deliberate trade, not an oversight: the
stateless alternative `K(n) = HKDF(K_root, n)` gives up backward secrecy
entirely. But the two mechanisms have opposite resync stories, and an
implementer should expect that rather than discover it.

**The schedule.** "Once per day" needs both ends to agree on
when the day turns, and devices sleep through most of it. The superframe counter is
the only shared clock, so rotation should be indexed by a counter threshold —
`superframe / 43200` at 2 s per superframe — rather than by wall time. A device
waking after a long sleep computes which generation is current and ratchets forward
to it. The RTC is not enabled on either core today; indexing by the counter means
it does not need to be.

A grace period alone is not enough — see the confirmation rule above.

## The replay floor belongs to a key, not to a device

**Rule, recorded before the hub's key store exists, because this is exactly the
kind of assumption that is cheap to fix now and silent later:**

> A persisted replay floor is scoped to the **session key**. Installing a new
> session key clears it.

A floor makes a receiver deaf to any counter below it. Scoped to the *device*,
a floor left over from a previous pairing — or from a hub whose counter has since
restarted — locks out the very device that just paired, permanently, with no
symptom but silence. It fails closed, which is the safe direction, but silently
and forever, which is the worst shape of safe.

Scoping it to the key is sound because a fresh pairing creates a fresh nonce
space: counters seen under the old key cannot be replayed under the new one, so
the old floor carries no information worth keeping.

The **transmit** floor is the opposite case and is carried forward across
pairings: keeping it only skips counter space, which is free, and skipping is the
conservative direction.

Found on the device side, where a stored floor of 5000 correctly refused a
legitimate frame at 1001 and the mechanism was right while the model was wrong.

## Storage

Long-term keys must survive reset; session keys need not and should not.

`cfg save` / `cfg load` exist in the CLI as **stubs**. There is no key store, no
wear levelling and no flash region reserved. This is unbuilt work, and it is on the
critical path — a hub that forgets its devices on every power cycle is not a hub.

Note the honest limit from [the threat model](threat-model.md): keys in internal
flash, no secure element, no readout protection configured. Physical possession of
the hub is game over, by design and not by oversight.

## See also

- [wire-crypto.md](wire-crypto.md) — the exact derivation rules
- [crypto-architecture.md](crypto-architecture.md)
