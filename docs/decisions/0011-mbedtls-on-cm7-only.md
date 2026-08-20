# ADR-0011 — mbedTLS as the one crypto library, on CM7 only

**Status:** Accepted. Vendored at 3.6.7 and verified on target.
**Date:** 2026-08-20

## Context

Two needs converge:

1. **The radio** needs ECDH ([ADR-0010](0010-p256-over-x25519.md)), HKDF and
   AES-GCM. None of it is implemented yet — no library has been chosen, so nothing
   is locked in.
2. **Secure Ethernet access is a stated goal** ([network/tls.md](../network/tls.md)).

The decisive technical fact: **lwIP ships `altcp_tls_mbedtls`**. The TLS glue in
the Cube FW package is written against mbedTLS specifically. Choosing anything else
for the radio would mean carrying two crypto libraries in one image.

There is already a dead ST CryptoLib scaffold in the tree —
`CM7/Core/Src/cmox_low_level.c`, referenced by no build file and compiled by
nothing — which is exactly how a project ends up with two.

Hardware, verified against both HAL driver sets:

| Primitive | H755 (hub) | WL55 (device) |
|---|---|---|
| AES | CRYP | CRYP/AES |
| SHA-256 | HASH | **none** |
| Elliptic curve | **none** | PKA |
| TRNG | RNG | RNG |

## Decision

**mbedTLS is the project's single crypto library, and it runs on CM7 only.**

- **CM7** — mbedTLS: P-256 ECDH, HKDF-SHA256 (HASH hardware), and TLS for Ethernet.
- **CM4** — no crypto library. Per-frame AES-128-GCM and the hop PRF go directly to
  CRYP through the HAL.
- **Devices** — no mbedTLS. They implement the same
  [wire format](../security/wire-crypto.md) with PKA, their AES block, and a small
  software SHA-256. See [ADR-0012](0012-wire-format-is-the-contract.md).

Session keys pass CM7 → CM4 through the [SRAM4 mailbox](0002-sram4-mailbox.md).

## Why CM7 only

This is the non-obvious half, since the radio lives on CM4.

**A P-256 scalar multiplication is tens of milliseconds of software arithmetic on a
Cortex-M4. A TDMA slot is milliseconds.** No amount of care makes a software point
multiplication fit inside a slot.

It does not have to. Asymmetric operations happen at **pairing** (an operator
action) and at **rotation** (once a day). Neither is time-critical. On CM7 under
FreeRTOS they can be preempted and nothing depends on their latency.

CM4 keeps only per-frame symmetric work, which is a CRYP hardware operation and
genuinely fits in a slot — and CM4 stays as it is: ~32 KB, no scheduler, no dynamic
allocation, small enough to reason about against a deadline.

## Consequences

- One crypto library: one AES to keep straight, one attack surface, one thing to
  update.
- CM4 gains no heap and no library. [ADR-0001](0001-dual-core-split.md)'s small
  auditable radio core survives.
- Flash is not a concern — CM7 uses ~129 KB of a 1 MB bank.
- ~~The IPC mailbox must gain sequence numbers first.~~ Done —
  [ADR-0016](0016-sequence-numbered-ipc-ring.md). The key-install path can be
  added as another request type.
- Session keys transit SRAM4, on-die and never on an external bus.
- Peripheral ownership follows: CRYP to CM4, HASH to CM7 —
  [ADR-0013](0013-crypto-peripheral-ownership.md).
- `cmox_low_level.c` should be **deleted**.

## Version: 3.6.7, and why not 4.x

The Cube FW package ships **mbedTLS 2.16.2**, dated 2019 and long past end of
life. Vendored instead as a submodule at **v3.6.7**, matching the `rfm69_lib`
habit.

3.6 rather than the newer 4.1 LTS, despite 4.1 being supported longer (March 2029
against March 2027, per the project's own `BRANCHES.md`):

- **4.x has no `include/mbedtls` crypto headers at all.** All crypto moved to a
  separate TF-PSA-Crypto repository and the legacy API is gone.
- That API is exactly what **ST's hardware `*_alt` files** implement
  (`aes_alt.c`, `sha256_alt.c`). Under 4.x, routing CRYP and HASH into mbedTLS
  would need a PSA driver, which ST does not ship for this part — so 4.x would
  cost the hardware acceleration this project explicitly prefers.
- lwIP's `altcp_tls_mbedtls` glue targets the legacy API too.

**March 2027 is a real horizon**, not a distant one, and migrating to a 4.x LTS
is known future work. It is the right trade today because 3.6 delivers hardware
acceleration and a working TLS path, and 4.x delivers neither without
substantial extra work.

Costs **31.7 KB of flash** for P-256 ECDH, HKDF-SHA256, AES-128-GCM and CTR_DRBG.

This is a **deliberate, narrow exception** to [ADR-0003](0003-cubemx-source-of-truth.md).
That rule exists so the GUI never disagrees with the firmware; here the generator
can only offer a version that should not be used. Confirm the current LTS version
at integration time rather than trusting this page.

## Verified on target

`crypto` on the console, 5/5: DRBG seed and draw, HKDF against RFC 5869 test case
1, AES-128-GCM round trip plus tamper rejection, P-256 ECDH with point validation
and off-curve rejection, and the [wire v1 interop vectors](../security/wire-crypto.md).

Three integration defects, each silent, are worth carrying:

- **The config header must not be called `mbedtls_config.h`.** `build_info.h`
  sits in the same directory as mbedTLS's own file of that name, so the quoted
  include resolves there first and your settings are ignored with no warning.
  Ours is `openhub_mbedtls_config.h`.
- **mbedTLS allocates with `calloc` and frees with `free`.** `newlib_stubs.c`
  overrides `malloc` and `free` to reach the FreeRTOS heap but **not `calloc`**,
  so mbedTLS allocated from the newlib heap and freed into the FreeRTOS one.
  heap_4's `configASSERT` then fired on a foreign block header — and since
  `configASSERT` is `taskDISABLE_INTERRUPTS()` plus an empty loop, it presents as
  a live core with a frozen tick and **no fault recorded**. Both halves are now
  pinned to one heap.
- **The 15 KB FreeRTOS heap is too small** for bignum work; raised to 64 KB, and
  cliTask's stack to 2048 words.

Seeding is lazy rather than at task start: entropy gathering must never hold up
the console, because the console is how a failure in it gets diagnosed.

## Alternatives rejected

**mbedTLS on both cores, built twice** — full config on CM7, crypto-only on CM4.
Adds a second build configuration and a heap to the real-time core to buy nothing
the mailbox does not already provide.

**A small dedicated library (tinycrypt, micro-ecc) for the radio, mbedTLS for TLS.**
Two crypto libraries in one image: two AES implementations, twice the audit surface,
more flash than one library serving both.

**ST X-CUBE-CRYPTOLIB (CMOX) for everything.** Hardware-aware and ST-supported, but
lwIP's TLS glue is not written against it — adopting it means writing that glue.
The dead `cmox_low_level.c` is the remains of this path.

**Hand-rolled P-256.** Not a reasonable trade for a security project.

## See also

[security/crypto-architecture.md](../security/crypto-architecture.md)
