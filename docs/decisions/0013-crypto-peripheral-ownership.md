# ADR-0013 — CRYP to CM4, HASH to CM7, RNG shared

**Status:** Proposed
**Date:** 2026-08-20

## Context

The H755 has one CRYP, one HASH and one RNG, all in the D2 domain and all
addressable from either core. [ADR-0011](0011-mbedtls-on-cm7-only.md) puts
symmetric radio crypto on CM4 and TLS plus asymmetric crypto on CM7, so both cores
have a legitimate claim on the accelerators.

CRYP is **stateful**: algorithm, key and IV are configured, then data is processed.
`hop_prf_aes` already has to `HAL_CRYP_GetConfig` / `SetConfig` to borrow it for an
ECB block.

## Decision

| Peripheral | Owner | Reason |
|---|---|---|
| **CRYP** | CM4, **exclusive** | per-frame AES-GCM must not contend with anything |
| **HASH** | CM7 | TLS handshake hashing and HKDF |
| **RNG** | shared, under `HSEM_RNG` | both cores legitimately need entropy |

CRYP and HASH are separate peripherals, so this split has **no contention at all**.
TLS bulk encryption on CM7 therefore uses mbedTLS's software AES.

## Consequences

- The radio's per-frame crypto has a private accelerator and a deadline it can meet
  without arbitration.
- **TLS AES is software.** On a 480 MHz M7 at home-LAN data rates this is not a
  limitation — the hub's traffic is sensor readings, not bulk transfer.
- CM7 gets hardware SHA-256, which serves both the TLS handshake and the HKDF used
  for [key derivation and rotation](../security/key-lifecycle.md).
- RNG stays shared and needs its semaphore: two cores reading `RNG_DR` concurrently
  can each get half a conditioned word.
- **A trap waiting in CM4's per-frame GCM**, found on the device side before this
  code exists here: `HAL_CRYP_Decrypt` in GCM mode does not mask the unused bytes
  of a **partial final word**, while encrypt does. Stale bytes past the payload
  then corrupt the tag on decrypt only, so every length not divisible by four
  fails while the ciphertext is byte-perfect — which over the air looks like a
  radio fault. Zero the input buffer before the copy. The
  [23-byte wire vector](../security/wire-crypto.md) exists to catch it.
- **Still to fix within CM4:** `hop_prf_aes` (ECB) and per-frame AES-GCM will both
  use CRYP. Even without cross-core contention these two must be serialised
  deliberately, since one reconfigures the peripheral the other depends on.
- **The RNG is now safe to draw from**, but only through the guarded service:
  `SEIS` latches spontaneously on this part and the HAL cannot report it. See
  [ADR-0015](0015-guarded-rng-access.md) and
  [security/entropy.md](../security/entropy.md).

## Alternatives rejected

**Share CRYP between cores under a semaphore.** Would give TLS hardware AES. Rejected
because CRYP is stateful and a shared stateful accelerator across two cores with
different scheduling models is a defect waiting to happen — one core's
save/restore bug silently corrupts the other's output, and the symptom is
ciphertext that decrypts wrong somewhere else entirely.

**CRYP to CM7, software AES on CM4.** Backwards: the deadline-bound core is the one
that needs the accelerator.

**Both accelerators to CM7, all crypto there.** Would mean shipping every frame's
payload across the mailbox for encryption and back, adding IPC latency inside a
slot.

## See also

[security/crypto-architecture.md](../security/crypto-architecture.md),
[architecture/ipc.md](../architecture/ipc.md)
