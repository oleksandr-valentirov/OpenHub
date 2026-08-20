# Host unit tests

Code that does not touch hardware is compiled for the host and tested there. It is
a small share of the firmware, but it is the share where a silent logic error would
be hardest to find on target.

```bash
make -C Common/test check              # hop sequence
make -C CM4/rfm69_lib/test check       # RFM69 driver
```

## Hop sequence — `Common/test/test_hop.c`

`hop.c` has no platform dependency by design: the PRF is injected as a callback
([radio/hopping.md](../radio/hopping.md)), so the host substitutes its own.

Properties asserted:

| Test | Why it matters |
|---|---|
| every cycle is a permutation | flat channel occupancy — the reason for the shuffle |
| a jump of N equals stepping N times | statelessness after sleep |
| a different key gives a different sequence | the key actually keys it |
| zero immediate repeats | an interferer must not kill two frames running |
| argument validation | `count` outside 2..64 is rejected |

These are **properties, not golden outputs**. A golden vector would pass while the
distribution was wrong; the permutation check is what actually catches the
modulo-bias failure mode that the earlier design had.

## RFM69 driver — `CM4/rfm69_lib/test/`

Nine groups run against a fake SPI register file (`fake_spi.h`), which is possible
only because the driver reaches the chip through injected callbacks
([radio/driver.md](../radio/driver.md)).

Unit conversions are pinned to the values the old driver's hardcoded magic numbers
produced — `frf(868 MHz) == 14221312`, `bitrate(9600) == 0x0D05`. That makes the
rewrite provably behaviour-preserving in the arithmetic, which was the part with no
other way to check it.

The fake also catches the class of bug that broke the old driver: a whole-register
write that clears bits another call set is visible in the register file
immediately.

## Missing

**Crypto test vectors.** Hub and device will be independent implementations of
[the wire format](../security/wire-crypto.md), and they need a shared fixed set of
vectors — known keys, known shared secret, known ciphertext and tag — checked on
both sides. This should exist *before* either side writes crypto code, and belongs
in `Common/test/`.
