# ADR-0005 — Rewrite the RFM69 driver around injected I/O

**Status:** Accepted
**Date:** 2026-08-20

## Context

The `rfm69_lib` submodule held a driver with defects that made the radio's
behaviour untrustworthy rather than merely limited:

- **No register shadows.** Whole-register writes meant setting a DIO mapping
  silently cleared the mode bits, and setting a mode silently cleared the DIO
  mapping. The symptom was intermittent and looked like a hardware fault.
- **Unmasked read addresses.** `rfm69_read` did not mask with `0x7F`, so a read
  address with the top bit set was a **write** — the driver could corrupt the
  register it meant to inspect.
- **`__weak` no-op SPI stubs.** A missing platform binding produced a driver that
  silently did nothing instead of a link error.
- **Magic numbers throughout**, with no way to check the unit conversions.

Patching these individually would leave the structure that produced them.

## Decision

Rewrite. The driver reaches the chip only through `rfm69_io_t` — five callbacks:
`transfer`, `select`, `reset`, `delay_us`, `micros`.

Registers written from more than one place are shadowed in `rfm69_dev_t` and always
read-modify-written. Settings are expressed in physical units
(`rfm69_set_carrier_hz`, `rfm69_set_bitrate`, …), with the conversions exposed so
tests can assert on them.

## Consequences

- **The driver runs on a host against a fake register file.** This is the actual
  justification: there is no second board and an RTL-SDR cannot answer, so without
  host tests this layer would have no test at all. Nine test groups now exist.
- Conversions are pinned to the values the old magic numbers produced —
  `frf(868 MHz) == 14221312`, `bitrate(9600) == 0x0D05` — so the rewrite is provably
  not a behaviour change in the arithmetic.
- A missing platform binding is a link error.
- One indirection per bus operation. Irrelevant against SPI transfer time.
- The submodule is now a hub-specific component. It was briefly justified as
  "portable to both projects", which was wrong — sensor devices have their own
  radio silicon and will never run this code. See
  [ADR-0012](0012-wire-format-is-the-contract.md).

## Alternatives rejected

**Patch the existing driver.** Would fix the listed bugs and leave whole-register
writes as the house style, so the next feature reintroduces the same class of bug.

**Use a third-party RFM69 library.** The available ones are Arduino-shaped, assume
a blocking `digitalWrite` world, and none is testable on a host.

## Consequences still open

Some paths in `radio.c` still poll DIO lines without a timeout. A chip that stops
responding hangs the superloop until IWDG2 resets the core — bounded, but the
watchdog covering for the driver.

## See also

[radio/driver.md](../radio/driver.md), [testing/host-tests.md](../testing/host-tests.md)
