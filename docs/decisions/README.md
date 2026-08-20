# Decision records

One decision per file. Each records the context that forced the choice, what was
decided, what it costs, and what lost.

**Accepted records are not rewritten.** A decision that stops holding gets a new
record that supersedes it, so the reasoning behind the old choice stays readable —
the most expensive thing to lose is *why* something was done, not what.

Status values: **Accepted** (decided and in the code), **Proposed** (decided on
paper, nothing implemented), **Superseded** (replaced, with a link forward).

| # | Decision | Status | Area |
|---|---|---|---|
| [0001](0001-dual-core-split.md) | CM7 runs the application, CM4 runs the radio | Accepted | [architecture](../architecture/dual-core.md) |
| [0002](0002-sram4-mailbox.md) | Cross-core IPC through SRAM4 and hardware semaphores | Accepted | [architecture](../architecture/ipc.md) |
| [0003](0003-cubemx-source-of-truth.md) | The `.ioc` is the source of truth | Accepted | [architecture](../architecture/build-and-generation.md) |
| [0004](0004-reference-libraries-not-vendor.md) | Reference HAL and middleware, do not vendor them | Accepted | [architecture](../architecture/build-and-generation.md) |
| [0005](0005-rewrite-rfm69-driver.md) | Rewrite the RFM69 driver around injected I/O | Accepted | [radio](../radio/driver.md) |
| [0006](0006-microsecond-timebase.md) | Free-running 32-bit microsecond timebase on TIM2 | Accepted | [radio](../radio/tdma.md) |
| [0007](0007-phy-profile.md) | 25 kbps GFSK with whitening, one 1% sub-band | Accepted | [radio](../radio/phy.md) |
| [0008](0008-keyed-shuffle-hopping.md) | Hop by keyed shuffle indexed by the superframe counter | Accepted | [radio](../radio/hopping.md) |
| [0009](0009-fixed-join-channel.md) | A reserved join channel and an operator-opened window | Accepted | [radio](../radio/joining.md) |
| [0010](0010-p256-over-x25519.md) | P-256 rather than X25519 | Accepted | [security](../security/crypto-architecture.md) |
| [0011](0011-mbedtls-on-cm7-only.md) | mbedTLS as the one crypto library, on CM7 only | **Proposed** | [security](../security/crypto-architecture.md) |
| [0012](0012-wire-format-is-the-contract.md) | Hub and device share a wire format, not code | Accepted | [security](../security/wire-crypto.md) |
| [0013](0013-crypto-peripheral-ownership.md) | CRYP to CM4, HASH to CM7, RNG shared | **Proposed** | [security](../security/crypto-architecture.md) |
| [0014](0014-console-on-vcp.md) | Console on the ST-Link VCP, interrupt-driven, no stdio | Accepted | [network](../network/ethernet.md) |
| [0015](0015-guarded-rng-access.md) | All RNG access goes through a guarded service | Accepted | [security](../security/entropy.md) |
| [0016](0016-sequence-numbered-ipc-ring.md) | Sequence-numbered rings replace the single-slot mailbox | Accepted | [architecture](../architecture/ipc.md) |
| [0017](0017-rotation-deletion-is-confirmation-driven.md) | Key deletion is confirmation-driven, never schedule-driven | Accepted | [security](../security/key-lifecycle.md) |
| [0018](0018-compressed-points-on-the-wire.md) | Compressed SEC1 points on the wire | Accepted | [security](../security/wire-crypto.md) |
| [0019](0019-lse-disciplined-timebase.md) | Discipline the timebase against LSE, by averaging rather than tracking | Accepted | [radio](../radio/timebase.md) |
| [0020](0020-device-triggered-quiesce.md) | Pairing quiesce is device-triggered, bounded and announced with a resume time | Accepted | [radio](../radio/pairing.md) |
| [0021](0021-hub-initiated-pairing.md) | pair_v3 changes discovery, not the key exchange; the operator enrols a public key | Accepted | [radio](../radio/pairing.md) |

## Template

```markdown
# ADR-NNNN — Title

**Status:** Accepted | Proposed | Superseded by ADR-MMMM
**Date:** YYYY-MM-DD

## Context
What made this a question. Constraints, measurements, what was already true.

## Decision
What was chosen, stated so it can be checked against the code.

## Consequences
What this costs and what it now forces. Including the bad parts.

## Alternatives rejected
Each with the reason it lost.
```
