# ADR-0018 — Compressed SEC1 points on the wire

**Status:** Accepted. The device side measured decompression at **13 ms** on the
PKA against 103 ms for the scalar multiply that follows — 13% on an operation
that happens rarely.
**Date:** 2026-08-20

Reverses the encoding choice recorded in
[wire-crypto.md](../security/wire-crypto.md) and referenced by
[ADR-0010](0010-p256-over-x25519.md). The curve is unchanged.

## Context

The original rule was: uncompressed points, `0x04 || X || Y`, 65 bytes, because
"point compression costs a modular square root on a device that has no spare
cycles for it."

That reasoning weighed device cycles and never weighed the hub's radio. **The
RFM69 FIFO is 66 bytes**, and `build_frame` spends byte 0 on the length, leaving
65 for payload. A pairing frame carrying a 65-byte key plus any header at all
does not fit in a single FIFO load.

The device side proposed a 73-byte pairing frame, which is invisible to them: the
SX126x has a 256-byte buffer. This is a hub-side constraint they cannot see.

## Options

| | Frame size | Cost |
|---|---|---|
| Uncompressed, refill the FIFO mid-transmission | 77 B | a timing-critical service path on CM4 and an underrun failure mode |
| Uncompressed, split across two frames | 2 frames | reassembly state, and a second failure mode during pairing |
| **Compressed, `0x02\|0x03 \|\| X`** | **45 B** | one modular exponentiation on the receiver |

## Decision

Carry **compressed** points: 33 bytes, `0x02` or `0x03` followed by X
big-endian. The pairing frame becomes 45 bytes and fits one FIFO load with 20
bytes to spare.

Recovering Y is one modular exponentiation, because P-256's prime is
`p = 3 mod 4`, so `y = ±(x³ + ax + b)^((p+1)/4) mod p`.

## Consequences

- The pairing frame fits in one FIFO load, so CM4 needs no mid-transmission FIFO
  service and no reassembly. That removes a failure mode rather than adding one.
- Air time falls from ~28 ms to ~17 ms. Marginal against a 1% budget for
  something that happens once, but it is the right direction.
- **Point validation stays mandatory, and on this side nothing about it relaxes.**
  It is tempting to say a decompressed point is on the curve by construction. That
  holds only if the implementation *verifies* the square root, and **mbedTLS does
  not** — its own comment says so:

  > this method for extracting square root does not validate that w was indeed a
  > square so this function will return garbage in Y if X does not correspond to
  > a point on the curve

  So `mbedtls_ecp_point_read_binary` **returns success with a garbage Y** for an X
  that is not any point's x-coordinate. `mbedtls_ecp_check_pubkey()` afterwards is
  the only thing standing between an attacker-chosen X and an invalid-curve
  attack. It is load-bearing, not hygiene.

  The device's implementation squares and compares, so there the vector really is
  removed. Two implementations, two different mandatory sets — which is the case
  for stating this per-side rather than as a property of "compression".
- **mbedTLS 3.6 reads compressed points**, contrary to its reputation — via
  `mbedtls_ecp_sw_derive_y`. Verified on the target rather than assumed: writing
  a compressed point and reading it back reproduces both X and Y, and the result
  validates. 87 ms including the keygen.
- The vectors gain compressed encodings, which is another additive set.
- **Measured on the device: 13 ms**, two hardware modexps plus trivial field
  arithmetic. The FIFO-refill fallback is not needed.

## Testing the rejection path

A perturbed valid key is **not** a negative test: roughly half of all field
elements are valid x-coordinates, so flipping a bit lands back on the curve about
half the time and the test passes while proving nothing. The device side hit this
and nearly went looking for a decompression bug.

`wire_v3` carries `reject_compressed_nonresidue` — `0x02 || x=1`, since **x = 1
has no square root on P-256**. Both sides check the same value.

## Alternatives rejected

**Refill the FIFO during transmission.** The RFM69 supports it and the driver has
the threshold setter, so it is not exotic. Rejected as the first choice because it
puts a timing-critical loop on the core that also has to meet TDMA deadlines, and
it fails as an underrun — mid-frame, intermittently.

**Split the key across two frames.** Reassembly state and a partial-pairing
failure mode, during the one exchange that is hardest to debug.

**A smaller curve.** Not seriously considered; the curve is fixed by the device's
PKA ([ADR-0010](0010-p256-over-x25519.md)).

## See also

[security/wire-crypto.md](../security/wire-crypto.md), [radio/joining.md](../radio/joining.md)
