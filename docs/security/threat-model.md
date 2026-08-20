# Threat model

Written before the crypto, so the crypto can be judged against something.

## What is being protected

| Asset | Why it matters |
|---|---|
| Sensor readings | occupancy, habits, whether anyone is home |
| Device roster | how many sensors, of what kind, where |
| Long-term device keys | compromise means permanent impersonation |
| Session keys | compromise means reading or forging traffic until rotation |
| The hub's LAN position | it is a device with an IP inside a home network |

Sensor data from a home is **presence data**. A radio observer who learns nothing
but "the door sensor reported at 08:12" learns when the house empties. Traffic
existing at all is not secret — the frames are on the air — but their contents and
their mapping to devices should be.

## Adversaries

**Passive radio observer.** Cheap SDR, within a few hundred metres. Can record
everything transmitted, indefinitely, at no cost. *This is the realistic
adversary and the one the design is actually shaped around* — the project's own
[SDR bench](../testing/sdr.md) is proof of how little equipment it takes.

**Active radio attacker.** Can transmit: replay captured frames, forge beacons,
jam. Jamming is not defended against — it cannot be, at this power level. Replay
and forgery must be.

**Attacker on the LAN.** Another host on the home network reaching the hub's IP.
Motivates the [planned TLS](../network/tls.md); today the console and any network
service are unauthenticated.

**Someone with the hub in their hands.** Out of scope. There is no secure element,
no flash readout protection configured, and no attempt at anti-tamper. If the hub
is stolen, its keys are gone. This is stated so nobody assumes otherwise.

## Required properties

| Property | Mechanism |
|---|---|
| Confidentiality of readings | AES-128-GCM |
| Integrity and authenticity | GCM tag — a forged frame must not merely fail to decrypt, it must be *rejected* |
| Replay resistance | nonce bound to the superframe counter, plus a receive window |
| Device authentication | long-term device key provisioned out of band |
| Forward secrecy across rotation | daily ratchet — yesterday's key does not decrypt tomorrow |
| Traffic-pattern resistance | **not provided** — see below |

## Accepted weaknesses

Stated explicitly, because an unstated weakness is the one that surprises someone.

- **Traffic analysis works.** Frame timing, length and the fact of transmission are
  visible. Constant-rate padding would defeat it and is unaffordable inside a 1%
  duty cycle. An observer can count devices and see when they report.
- **The join beacon is unauthenticated** and always will be — it is the one frame
  readable without a key. A forged one can waste a joining device's time; it
  cannot complete a pairing, because pairing binds to an out-of-band key. See
  [radio/joining.md](../radio/joining.md).
- **Jamming is undefeatable** here. Hopping raises the cost of a *narrowband*
  interferer, which is the common accidental case; it does nothing against a
  deliberate wideband jammer.
- **No secure element.** Long-term keys live in internal flash.
- **The console is unauthenticated.** Physical USB access to the ST-Link VCP is
  full control of the hub.

## Non-goals

Anonymity of the network's existence. Resistance to a nation-state. Protection of
the hub against its owner.

## See also

- [crypto-architecture.md](crypto-architecture.md)
- [key-lifecycle.md](key-lifecycle.md)
