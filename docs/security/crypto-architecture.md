# Crypto architecture

**Status: implemented for the primitives, planned for the framing.** mbedTLS 3.6.7
is vendored and running on CM7, and the primitives are verified against shared
interop vectors (`crypto`, 7/7). What does not exist yet is the pairing exchange
and the sealed frame path that would use them.

## Hardware inventory

The two ends of the link have **complementary gaps**, and this drives everything
below.

| Primitive | STM32H755 (hub) | STM32WL55 (device) |
|---|---|---|
| AES | **CRYP** | **CRYP/AES** |
| SHA-256 | **HASH** | none — software |
| Elliptic curve | none — software | **PKA** |
| TRNG | **RNG** | **RNG** |

Verified against the HAL headers in both Cube FW packages: the H7 driver set has
`cryp`, `hash`, `rng` and **no `pka`**; the WL driver set has `cryp`, `pka`, `rng`
and **no `hash`**.

So neither side can do everything in hardware, and they are short of *different*
things. The hub does elliptic-curve maths in software; the device does SHA-256 in
software. There is no arrangement in which one library, hardware-accelerated,
serves both — which is the technical form of the conclusion in
[ADR-0012](../decisions/0012-wire-format-is-the-contract.md): **the shared thing
is the wire format, not the code.**

## Decision: mbedTLS on the hub, CM7 only

```
  CM7  (FreeRTOS, 480 MHz)        CM4  (superloop, 240 MHz)
  ------------------------        -------------------------
  mbedTLS                         no crypto library
    - P-256 ECDH   (software)     AES-128-GCM via CRYP (HAL)
    - HKDF-SHA256  (HASH hw)      hop PRF: AES-128-ECB via CRYP
    - TLS for Ethernet
                    |
                    +-- session keys --> SRAM4 mailbox --> CM4
```

Devices get **no** mbedTLS: they implement the same wire format with PKA, their
AES block and a small software SHA-256.

### Why mbedTLS rather than a second library

Secure Ethernet is a stated goal, and **lwIP ships `altcp_tls_mbedtls`** — the TLS
glue in the Cube FW package is written against mbedTLS specifically. Choosing
anything else for the radio would mean carrying two crypto libraries in one image:
twice the flash, twice the attack surface, and two implementations of AES to keep
straight.

mbedTLS also covers the whole radio set on its own — `mbedtls_ecdh_*`,
`mbedtls_hkdf`, `mbedtls_gcm` — and its P-256 is constant-time and widely reviewed.
Hand-rolling elliptic-curve arithmetic for a security project is not a reasonable
trade.

There is already a **dead CMOX scaffold** in the tree: `CM7/Core/Src/cmox_low_level.c`
is a template for ST's X-CUBE-CRYPTOLIB, referenced by no build file and compiled
by nothing. It should be deleted — leaving it invites exactly the two-library
outcome this decision avoids.

### Why CM7 only, and not on CM4 as well

This is the part that is not obvious, because the radio lives on CM4 and the radio
is what needs the keys.

**A P-256 scalar multiplication is ~160 ms of software arithmetic**, measured on
this hub's 480 MHz M7 (four of them in 656 ms via the `crypto` command). A TDMA
slot is measured in milliseconds. Running a key exchange inline on the radio core
would blow the slot grid several hundred times over.

For scale, the device side measured **103 ms** for the same operation in PKA
*hardware* on its 48 MHz M4 — so this is not a figure optimisation rescues.

But it does not have to be inline. The asymmetric operations happen at **pairing**
(an operator action) and at **rotation** (once a day). Neither is time-critical.
Putting them on CM7 under FreeRTOS, where they can be preempted and where nothing
depends on their latency, costs nothing and keeps CM4 as it is: ~32 KB, no
scheduler, no dynamic allocation, and small enough to reason about against a
deadline.

CM4 keeps only the per-frame symmetric work, which is a CRYP hardware operation
and genuinely fast enough to run inside a slot.

The alternative — building mbedTLS twice, once with a crypto-only config for CM4 —
was considered and rejected. It adds a second build configuration and a heap to
the real-time core to buy nothing that the mailbox does not already provide.

### Peripheral ownership

| Peripheral | Owner | Reason |
|---|---|---|
| CRYP | **CM4, exclusive** | per-frame AES-GCM must not contend with anything |
| HASH | **CM7** | TLS handshake hashing and HKDF |
| RNG | shared, under `HSEM_RNG` | both cores legitimately need entropy, through [the guarded service](entropy.md) |

CRYP and HASH are separate peripherals, so this split has **no contention at all**
— which is the point. TLS bulk encryption on CM7 therefore uses mbedTLS's software
AES. At sensor-hub data rates that is irrelevant; software AES-GCM on a 480 MHz M7
is well beyond what a home LAN link will ask of it.

Sharing CRYP between the cores under a semaphore was the alternative. It was
rejected because CRYP is **stateful** — `hop_prf_aes` already has to save and
restore the configuration to borrow it for ECB — and a shared stateful accelerator
across two cores with different scheduling models is a defect waiting to happen.

Recorded as [ADR-0013](../decisions/0013-crypto-peripheral-ownership.md).

## Open problem: the shipped mbedTLS is out of support

The Cube FW H7 package ships **mbedTLS 2.16.2**, dated 2019. The 2.16 line is long
past end of life. Shipping unmaintained crypto in a project whose entire premise is
not trusting someone else's security is a contradiction worth naming.

Two paths:

| | Take ST's 2.16.2 | Vendor current LTS as a submodule |
|---|---|---|
| Effort | none | write the config, check the lwIP glue |
| CubeMX integration | native | outside the generator |
| Security posture | unmaintained | supported |
| lwIP `altcp_tls` | matches | the 3.x API changed; needs the newer upstream glue |

**Recommendation: vendor the current mbedTLS LTS as a git submodule**, the way
`rfm69_lib` already is. The project has the submodule habit already, flash headroom
is ample (~890 KB free in bank 1), and the alternative is knowingly building on
crypto nobody patches.

This is a **deliberate exception** to the "anything CubeMX can generate must be
generated" rule in [ADR-0003](../decisions/0003-cubemx-source-of-truth.md). The
rule exists so the GUI never disagrees with the firmware; here the generator can
only offer a version that should not be used. The exception is narrow — mbedTLS
only — and belongs written down rather than discovered later.

Confirm the current LTS version at integration time rather than trusting this page.

## What has to exist before any of this is written

- ~~A sequence-numbered IPC ring.~~ Done — see
  [architecture/ipc.md](../architecture/ipc.md). Key install becomes another
  request type.
- **A key store.** See [key-lifecycle.md](key-lifecycle.md); `cfg save`/`load` is
  currently a stub.
- ~~A trustworthy RNG.~~ Done — see [entropy.md](entropy.md).
- **The wire format frozen.** [wire-crypto.md](wire-crypto.md) — devices are built
  in a separate effort and cannot track a moving target.

## See also

- [ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md)
- [network/tls.md](../network/tls.md)
- [threat-model.md](threat-model.md)
