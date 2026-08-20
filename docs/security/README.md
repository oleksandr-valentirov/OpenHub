# Security

The project exists because its author does not want sensor data held by a company
with closed software on a centralised server. That premise sets the bar: the hub
has to be defensible on its own, not merely private by policy.

| Page | Subject | Status |
|---|---|---|
| [threat-model.md](threat-model.md) | assets, adversary, what is out of scope | accepted |
| [entropy.md](entropy.md) | the hardware RNG and why every draw is checked | **implemented** |
| [crypto-architecture.md](crypto-architecture.md) | which library, on which core | **implemented** |
| [wire-crypto.md](wire-crypto.md) | the byte-level contract both ends implement | proposed |
| [key-lifecycle.md](key-lifecycle.md) | provisioning, exchange, rotation, storage | proposed |

**Nothing on the radio link is encrypted today.** The frames go out in the clear.
The primitives are in place and verified against interop vectors; what is missing
is the pairing exchange and the framing that would use them.
These pages describe the design that is being built toward, and are explicit about
the boundary between what runs and what is planned.

## The one-paragraph version

Symmetric crypto runs on CM4 in CRYP hardware, inside the radio's real-time loop.
Asymmetric crypto — the elliptic-curve key exchange — runs on CM7 under FreeRTOS
using mbedTLS, because it is slow, rare, and must never sit inside a TDMA slot.
Sensor devices share none of that code; what hub and device share is a wire
format specified down to the byte.

Decisions: [ADR-0010](../decisions/0010-p256-over-x25519.md),
[ADR-0015](../decisions/0015-guarded-rng-access.md),
[ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md),
[ADR-0012](../decisions/0012-wire-format-is-the-contract.md),
[ADR-0013](../decisions/0013-crypto-peripheral-ownership.md).
