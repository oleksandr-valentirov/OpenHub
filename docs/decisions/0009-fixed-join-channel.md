# ADR-0009 — A reserved join channel and an operator-opened pairing window

**Status:** Accepted
**Date:** 2026-08-20

## Context

[ADR-0008](0008-keyed-shuffle-hopping.md) keys the hop sequence with a secret
established at pairing. That creates a bootstrap problem: **a device with no key
cannot know where to listen.**

Pairing was also implemented as `RFM_add_device_routine`, a loop that blocked for
up to ten seconds waiting for a response — incompatible with a slot grid, which it
would stall for thousands of slot-times.

## Decision

Reserve grid slot **14 → 866.5 MHz** for joining. The hopping set becomes the other
**28** channels, and `hop_slot_to_grid` skips the reserved slot, so the two sets are
disjoint by construction.

The hub transmits there **only while an operator has a pairing window open**
(`device add <id> <fingerprint>`, 60 s), and then only every **second** superframe.

`RFM_open_pairing` sets a deadline and returns. The beacon goes out through the
ordinary transmit path; the window closes on its own deadline.

`radio_join_beacon_t` is 14 bytes and cleartext, carrying `net_id`, `hub_id`, the
superframe counter, flags and the hop-set size.

## Consequences

- A joining device parks on one known frequency instead of scanning — cheap for a
  battery node, and reliable.
- **0.21% duty cycle during a window, exactly zero outside one.** A permanent join
  beacon would cost that around the clock for nothing.
- Half rate rather than every superframe halves the cost while still letting a
  device find the hub within two superframes.
- **A power-cycled device does not need this channel at all.** It still has its key,
  and any data beacon carries the counter. The join channel serves only devices with
  no key, which is always a deliberate human action at both ends.
- Carrying the counter means the joiner is time-aligned before it has a key.
- The hop set drops from 29 to 28 channels. Immaterial.
- Pairing no longer blocks the radio.
- **The join beacon is unauthenticated and always will be** — it is the one frame
  readable without a key. Anyone can forge one. This is not fixable at this layer;
  authentication comes from the exchange that follows, which binds to a device key
  the hub obtained out of band. A forged beacon can waste a joining device's time
  and cannot complete a pairing. Accepted, not solved.

## Verification

Measured with the SDR bench, filtered to 866.5 MHz:

| | Packets |
|---|---|
| Window closed | **0** |
| Window open | **8**, spaced 4008 ms, 8.27 ms each |

4008 ms is exactly two 2004 ms superframes; 8.27 ms matches a 14-byte frame at
25 kbps with preamble and sync.

## Alternatives rejected

**Scan the whole band.** Slow and unreliable on a battery node, and it fails
silently when it fails.

**Unencrypted beacon on every channel.** Costs duty cycle on every channel forever,
and multiplies the surface for a forged beacon.

**A join channel at a band edge.** Rejected in favour of the middle of the band,
which leaves the most room for a cheap device's crystal error before the signal
falls outside 865–868 MHz.

## See also

[radio/joining.md](../radio/joining.md),
[security/key-lifecycle.md](../security/key-lifecycle.md)
