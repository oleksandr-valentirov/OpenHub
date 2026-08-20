# ADR-0012 — Hub and device share a wire format, not code

**Status:** Accepted
**Date:** 2026-08-20

## Context

An early justification for the [driver rewrite](0005-rewrite-rfm69-driver.md) was
that it would run "in both projects" — hub and sensor device. That was wrong, and
was corrected: the hub drives an RFM69 over SPI, while STM32WL55 devices have an
integrated SUBGHZ radio. The device will never run the RFM69 driver.

The same reasoning applies to crypto. mbedTLS is not going onto the sensor nodes:
it is far too large for a battery device, and the device's PKA already accelerates
the one operation that matters there.

The hardware gaps are **complementary**, which makes shared implementation
impossible even in principle:

| Primitive | Hub (H755) | Device (WL55) |
|---|---|---|
| SHA-256 | HASH hardware | **software** |
| Elliptic curve | **software** | PKA hardware |

Neither side can do everything in hardware, and they are short of *different*
things.

## Decision

**The contract between hub and device is the wire format, specified to the byte —
not shared source code.**

Genuinely shared code is limited to what is platform-free and identical by
definition:

- `Common/inc/radio_protocol.h` — frame layouts
- `Common/inc/hop.h`, `Common/src/hop.c` — the hop sequence, with the PRF injected
- the crypto wire format in [security/wire-crypto.md](../security/wire-crypto.md)

Everything below that — radio driver, AES, SHA-256, elliptic curve — is
per-platform and chosen for that platform's silicon.

## Consequences

- Each side uses the best implementation its hardware offers. The hub gets mbedTLS
  and hardware SHA-256; the device gets PKA and a compact software SHA-256.
- **Ambiguity in the spec becomes an interoperability bug**, and cannot be papered
  over by both sides calling the same function. Hence the byte-level rules in
  wire-crypto.md: SEC1 uncompressed points, X-coordinate-only shared secret,
  endianness stated per field type.
- **Shared test vectors are required, not optional.** Two independent
  implementations need known keys, a known shared secret, a known ciphertext and
  tag, checked on both sides. These should exist *before* either side writes crypto
  code — they are what lets the hub effort and the device effort proceed in parallel
  without integration guesswork.
- The wire format must be **frozen** before device firmware depends on it.
- `hop.c` stays free of platform dependencies, which is also what makes it host
  testable.
- Two implementations to maintain and to keep in step. Accepted — the alternative
  does not exist on this hardware.

## Alternatives rejected

**A shared crypto library on both.** mbedTLS is too large for the device, and no
single library uses hardware on both sides, because the hardware differs.

**A shared HAL abstraction over both radios.** The RFM69 and SUBGHZ differ in
register model, packet engine and control path. The abstraction would be thicker
than either driver and would obscure both.

## See also

[security/wire-crypto.md](../security/wire-crypto.md),
[radio/README.md](../radio/README.md)
