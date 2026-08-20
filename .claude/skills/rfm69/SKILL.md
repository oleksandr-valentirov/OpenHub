---
name: rfm69
description: Working with the RFM69/SX1231 radio on this hub - module variants, the registers whose reset values are wrong, SPI limits, and how to tell a dead receiver from an empty band. Use when the radio will not transmit, will not receive, corrupts frames, or when touching CM4/rfm69_lib or the PHY.
---

# The RFM69 on this board

`CM4/rfm69_lib` is a submodule with its own host tests (`make -C test check`).
The hub drives it over SPI1 from CM4. This file is the accumulated cost of
getting it working; **add to it whenever the part surprises you again.**

## The rule that produced most of this file

**Every register the driver depends on must be written by the driver.** A field
left at its reset value is not "not configured yet" - it is a configuration, and
the part obeys it. Four separate faults below are one register nobody named.

The corollary bites harder: **a function that correctly computes the bits it was
thinking about still leaves the rest of the register at whatever was there.**
`rfm69_rx_bandwidth_to_reg` returned a correct mantissa and exponent for years
and shared `RegRxBw` with `DccFreq`, which stayed 0. Every bandwidth test passed.

## Module variants, and why the hub was inaudible

**RFM69W / RFM69CW** bond **PA0** to the antenna. Max +13 dBm.
**RFM69HW / RFM69HCW** do **not** bond PA0. They route **PA1**, or PA1+PA2 for
+20 dBm. The reset value of `RegPaLevel` is `PA0On=1`, so a high-power module
out of reset transmits into a pin that goes nowhere.

**The boards are physically identical apart from the marking.** The hub ran
`RegPaLevel = 0x9F` (PA0, +13 dBm) for months and radiated a measured **-40 dBm
EIRP**. Every call returned `RFM69_OK`, every flag was correct, `PacketSent`
fired. The only symptom was range.

```
PA0 only        Pout = -18 + OutputPower    -18..+13   0x80 | level
PA1 only        Pout = -18 + OutputPower     -2..+13   0x40 | level   min level 16
PA1+PA2         Pout = -14 + OutputPower      2..+17   0x60 | level   min level 16
PA1+PA2 boost   Pout = -11 + OutputPower      5..+20   0x60 | level   + RegTestPa1/2, OCP off
```

`rfm69_set_power(dev, pa, dbm)` names the PA and writes `RegTestPa1`,
`RegTestPa2` and `RegOcp` on **every** call - leaving boost mode must put all
three back, or the next setting inherits them.

**How it was caught:** three independent instruments agreed on the radiated
power before anyone read the register - the peer's receiver, reciprocity against
its own bench node at the same distance and power, and the SDR, where the hub's
beacon sat 1.1 dB over the noise floor while the device's sat 43.9. That 43 dB
had been in a capture for weeks, noted as a possible antenna artifact.

## The receive front end: three registers, four hours

Bisected on hardware against a baseline proven to fail with the device
transmitting at -26 dBm. **Sufficiency arms against an all-broken baseline**, not
reversion arms - with two independently sufficient causes, every reversion arm
comes back green and reads as "none of them did it".

```
none fixed        0 of 15
DccFreq only      0 of 14      not sufficient
RssiThresh only   9 of 14      sufficient, and loses a third of frames
DAGC only        15 of 15      sufficient, and the actual fix
```

**`RegRssiThresh` (0x29), reset `0xFF` = -127.5 dBm.** That is ~19 dB *below*
this board's own measured noise floor, so the level condition is true from the
instant the receiver comes up. Set it near the part's sensitivity; the hub uses
-100 dBm, chosen against a measured -108 floor with bursts to -79.

**`RegTestDagc` (0x6F), reset `0x00`.** The legacy mode. Datasheet 3.4.4 asks for
`0x30` at every setting this part runs at. Also collapses the *rate* at which a
triggered RSSI measurement completes - by about 8000x - which is a second,
independent way to see whether it took effect.

**`RegRxBw` (0x19) `DccFreq`, bits 7:5, reset 0.** Sixteen times the recommended
corner. `RFM69_DCC_DEFAULT` is `0x80` (DccFreq 4).

## SPI: 6.25 MHz corrupts FIFO reads, and it is not a speed limit

**Quote SPI rates in Hz, never as a prescaler.** The divider is meaningless
without its kernel clock, and here that is PLL1Q at 200 MHz, set on the *other*
core three files away. `device spiloop` reads both halves off the peripheral -
the kernel clock from RCC, the divider from `SPI1->CFG1` MBR - and prints the
rate, so the number in a report is measured rather than inferred.

Measured on this board, 200 passes of a 57-byte pattern through the FIFO,
against the same read and write path on a 16-byte register block:

```
   1.5625 MHz   /128   FIFO clean          registers clean
   3.125  MHz   /64    FIFO clean          registers clean
   6.25   MHz   /32    FIFO 200/200 BAD    registers clean
  12.5    MHz   /16    FIFO clean          registers clean
  25      MHz   /8     FIFO clean          registers clean
```

**One rate fails, and it is not the fastest.** 25 MHz is two and a half times the
part's 10 MHz datasheet maximum and comes back clean; 6.25 MHz, comfortably
inside spec, corrupts about 45% of bytes. Reproduced four times, alternating
6.25 and 12.5 MHz between reflashes, identical every time.

**This is unexplained.** A clock that is simply too fast fails monotonically, and
this does not, so "the part cannot keep up" is wrong however plausible it reads -
that was the first explanation written here and the sweep destroyed it. Do not
replace it with another story until something measures the cause. A scope on
SCK and MISO at both rates is the obvious next step and has not been done.

The corruption itself: **bit 7 of roughly 45% of bytes, XOR exactly `0x80`, never
any other bit.** It happens *after* the packet engine has verified CRC, so
`crc err` reads 0 and the payload is silently wrong. On air it presented as a
pairing that failed on a fingerprint computed over a key one bit different from
the one sent, and as a `dev_id` of `a5a5a525` where `a5a5a5a5` was transmitted.

**The hub runs at 1.5625 MHz.** Not 12.5 or 25, which also pass: those are clean
against a cause nobody understands, and 12.5 is already above the part's rated
maximum. The slow rate costs about 3 us of worst-case beacon lateness, which
`timing` still does not flag.

`device spiloop [n]` reproduces all of this with nothing on air. **It ships with
its control** - the register-block arm - and that control is what makes the table
above readable in both directions. Without it, a failing FIFO arm is
indistinguishable from an invalid test, which is exactly how it was read for
half an hour before a rate sweep proved it had been right all along.

## Packet engine settings this project depends on

```
RegDataModul   0x02   packet mode, FSK, Gaussian BT 0.5
bit rate       25000 bps        RegBitrate 0x0500
deviation      25000 Hz         RegFdev    0x0199
RX bandwidth   100 kHz          RegRxBw    0x8A  (with DccFreq 4)
preamble       4 bytes          RegPreamble 0x0004
sync           68 65 6C 6C      RegSyncConfig 0x98, 4 bytes, 0 error tolerance
RegPacketConfig1 0x98           variable length, DcFree none, CrcOn,
                                CrcAutoClearOff, no address filter
RegPacketConfig2 0x02           AutoRxRestartOn
RegPayloadLength 0x40 = 64      the *maximum accepted* length in variable mode
power          PA1, +13 dBm     RegPaLevel 0x5F
```

**`CrcAutoClearOff` matters for diagnosis.** Without it the part discards a
bad-CRC frame before `PayloadReady` and "arrived broken" is indistinguishable
from "nothing arrived". With it, `rx_frame_ready` sees `PayloadReady` without
`CRC_OK`, counts it and drains the FIFO - so `crc err` becomes a real number.

**No whitening.** The SX1231 and the device's SX126x use different LFSRs.

**`SyncAddressMatch` is a level, not a pulse.** It stays asserted until the mode
changes, so *an edge* is what a frame is. Polling the level counts one arrival
hundreds of times and produces a number that looks like a busy healthy channel.

## RSSI: the register is a latch, not a meter

`RegRssiValue` is refilled only when the receiver comes up or when `RssiStart`
(`RegRssiConfig` bit 0) is written and `RssiDone` (bit 1) comes back. **Polling it
returns the sample taken at RX entry, forever** - peak and floor read identical
and the band looks flat and quiet.

- `rfm69_measure_rssi()` triggers and waits. Use it for surveying a channel.
- `rfm69_get_rssi()` reads the latch. Correct **only** straight after a packet,
  where the latch holds that packet's own level.
- **Ordering is load-bearing.** A triggered measurement destroys the latch, so
  sample only after frame handling and only with `SyncAddressMatch` clear.
  Sampling first gives a working meter that silently overwrites the one number
  with no second chance at it.
- The driver returns a **negative** half-dB value. `x2 / 2` is dBm; negating it
  again yields a positive "dBm" that looks plausible and is wrong.

## Telling a dead receiver from an empty band

The counters exist in this order because each one below is *below* the last:

```
sync N, crc N, frames N      the radio layer works
sync N, crc N, frames 0      frames arrive and fail the checksum
sync N, crc 0, frames 0      one started and never finished
sync 0                       nothing detected: frequency, sync word, preamble,
                             range - or the receiver was never on
level peak >> floor          something is radiated whether or not this part
                             can read it. This is the only measurement that
                             sits below the sync word.
```

**Count the receive windows too.** `sync 0` cannot distinguish "nothing was
transmitted" from "my receiver never opened", and a window gated on several
conditions not opening is the likely case, not the exotic one.

**A counter that has never been non-zero is indistinguishable from one that
cannot be.** `sync` read 0 for the entire life of this project before the front
end was fixed. Get its positive case seen once before a zero from it is evidence.

## Transmitting

**Leave RX before touching the FIFO.** Writing it while the receiver is running
mixes the outgoing frame with what the packet engine has collected and the
transmit fails - seen as `tx err`. The standby transition lives **inside**
`transmit()` in `radio.c`, not at the call sites: three functions call it and the
one that forgot was the one whose failure lost a `PAIR_RSP`.

Everything is one FIFO write including the length byte, so a frame plus its
length must fit 66 bytes.

## Reading registers back

`device dump <reg>` on the CLI, over IPC to CM4. **Read the register, do not
trust the call that wrote it** - `rfm69_set_power_dbm` returned success for
months while selecting an unbonded PA.

Each `device dump` is a separate command a third of a second apart, so **three
dumps of `RegFrf` bytes 0x07/0x08/0x09 are three different superframes** on a
hopping radio and do not form a valid frequency. Read multi-byte state in one
burst or on a stopped grid.
