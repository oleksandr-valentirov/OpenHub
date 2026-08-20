# ADR-0007 — 25 kbps GFSK with whitening, inside one 1% sub-band

**Status:** Accepted
**Date:** 2026-08-20

## Context

The original configuration measured **3.5% transmit duty cycle** on air. The
868 MHz sub-band it used allows **1%** (ERC REC 70-03 / ETSI EN 300 220). The
firmware was not merely inefficient, it was non-compliant.

Three causes:

- `DcFree=Manchester` **doubles air time** to obtain DC balance that whitening
  gives for free.
- 9.6 kbps stretched every frame unnecessarily.
- A 449-entry channel table spanning **four sub-bands with different duty-cycle
  limits**, stepping 15.625 kHz — narrower than the signal itself, so adjacent
  entries overlapped and it was not really 449 channels.

## Decision

| Parameter | Value |
|---|---|
| Modulation | GFSK |
| Bit rate | 25 kbps |
| Deviation | 25 kHz |
| RX bandwidth | 100 kHz |
| DC-free coding | **whitening** |
| Band | 865.1 MHz base, 100 kHz spacing, 29 slots, all inside 865–868 MHz |

## Consequences

- Duty cycle fell from ~3.5% to **~0.4%**, with room for the TDMA traffic still to
  be added.
- One sub-band means **one duty-cycle limit** to budget against. The previous plan
  required tracking which sub-band each transmission landed in — a bookkeeping
  problem that had not been solved and would have been easy to get wrong.
- 100 kHz spacing exceeds the occupied bandwidth, so channels are genuinely
  independent.
- Modulation index 2 tolerates ±20 ppm of crystal error at both ends, which is the
  margin cheap battery nodes need.
- 29 channels instead of a nominal 449. The nominal figure was never real.
- **The compliance limit, not throughput or range, is the binding constraint on the
  protocol.** Every later design decision is budgeted against it.
- `tools/sdr/dutycycle.py` exits non-zero over the limit, so this is enforced rather
  than remembered.

## Still open

There is **no in-firmware duty-cycle governor**. The hub trusts its schedule rather
than counting its own air time and refusing to transmit. Until that exists, any
change to slot timing has to be re-measured with the SDR.

## Alternatives rejected

**Keep Manchester.** Doubles air time for no benefit whitening does not provide.

**Higher bit rate (50–100 kbps).** Less air time still, at the cost of receiver
sensitivity and therefore range — and 0.4% is already comfortably inside budget.
Not worth the link margin.

**Stay wideband across four sub-bands.** More channels, at the cost of tracking a
different limit per transmission.

## See also

[radio/phy.md](../radio/phy.md), [testing/sdr.md](../testing/sdr.md)
