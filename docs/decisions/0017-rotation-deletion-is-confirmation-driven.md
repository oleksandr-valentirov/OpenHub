# ADR-0017 — Key deletion is confirmation-driven, never schedule-driven

**Status:** Accepted
**Date:** 2026-08-20

Refines [key-lifecycle.md](../security/key-lifecycle.md). Raised by the device-side
effort while reviewing the rotation design before implementing it.

## Context

The daily rotation is an HKDF ratchet: `K(n+1) = HKDF(K(n), "openhub/v1/rotate")`.
No radio exchange, so it costs no air time — which matters inside a 1% duty
cycle ([radio/phy.md](../radio/phy.md)).

The design indexed the rotation generation off the superframe counter. **That
counter reaches an unpaired or resyncing device in the cleartext join beacon**,
which is unauthenticated by necessity — it is the one frame readable without a
key ([radio/joining.md](../radio/joining.md)).

### The attack

An attacker with a transmitter and no key material announces a counter far in the
future. A device that indexes its ratchet on that counter fast-forwards, deletes
the superseded key, and can never talk to the hub again.

That is **permanent de-pairing of every device in radio range, from one
transmission**, with no ciphertext and no interaction with the hub. It is
strictly worse than jamming: jamming stops when the attacker leaves, and this
does not — recovery needs a physical re-pair per device.

The root cause is not the ratchet. It is **unauthenticated input driving
irreversible key deletion**.

### The second problem: the ratchet is stateful

[ADR-0008](0008-keyed-shuffle-hopping.md) made the hop sequence deliberately
stateless, because a node that slept a thousand superframes must be able to
compute the current channel directly. A ratchet is the opposite: `K(n)` cannot be
evaluated without having walked to it.

The stateless alternative, `K(n) = HKDF(K_root, n)`, gives up backward secrecy
entirely — one compromise exposes every generation of the epoch. That is a worse
trade, so the ratchet stays. But the two mechanisms now have **opposite resync
stories**, and that has to be written down rather than discovered.

## Decision

1. **Deletion is confirmation-driven.** A device keeps `K(n)` until a frame has
   authenticated under `K(n+1)`. The schedule proposes; authentication disposes.
2. **The hub accepts the previous generation during changeover.** This is what
   makes rule 1 work, and it is a hub-side obligation, so it belongs in the
   protocol rather than in device code.
3. **The rotation index comes only from an authenticated frame.** The cleartext
   join beacon may tell a device it is behind; it must never move the ratchet.
4. **Fast-forward is bounded.** A device refuses to walk more than one epoch of
   generations in one go. Beyond that it treats itself as desynchronised and
   re-pairs deliberately rather than destroying its own key.
5. **Retention is bounded too.** The hub keeps the previous generation for a
   bounded number of periods, not indefinitely — otherwise a device that never
   confirms keeps an old key alive forever and erodes the forward secrecy the
   ratchet exists to provide. Past that bound, the device must re-pair.

An epoch is the interval between fresh ECDH re-pairings (target: monthly,
device-initiated, in its own window). That bounds the maximum walk to roughly 30
HKDF steps, which is cheap, and it is what makes rule 4 a small number rather
than an arbitrary one.

## Consequences

- **Availability is preserved against an attacker who can only transmit.** The
  cheapest possible attack no longer costs a physical re-pair.
- Forward secrecy loses exactly one rotation period of exposure, because one
  previous key is retained. That is a far smaller loss than the availability
  failure it prevents.
- The hub carries two keys per device during changeover — 128 keys at 64 devices,
  which is nothing.
- Replay resistance must not weaken while two keys are live: the nonce is bound
  to the superframe counter and the receive window still applies per generation
  ([security/wire-crypto.md](../security/wire-crypto.md)).
- Rotation needs no RTC. Indexing by counter threshold rather than wall time was
  already the plan and stays.
- A device offline longer than an epoch must re-pair. Accepted: that is a
  deliberate human action for a device that has been away a month.

## Alternatives rejected

**Index the ratchet on the superframe counter as received.** The original design.
Turns one forged frame into permanent, physical-intervention-only failure.

**Stateless `K(n) = HKDF(K_root, n)`.** Removes the walk and the resync problem
entirely, and gives up backward secrecy — a device compromise would expose every
past generation in the epoch.

**Delete on a timer with a grace period.** Better than immediate deletion, and
still schedule-driven: a grace period can be outrun by an attacker who simply
waits it out, and by a device that sleeps through it.

**Never delete old keys.** No availability failure, and no forward secrecy.

## See also

[security/key-lifecycle.md](../security/key-lifecycle.md),
[security/threat-model.md](../security/threat-model.md)
