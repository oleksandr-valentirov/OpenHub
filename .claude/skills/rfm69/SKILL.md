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
`0x30` at every setting this part runs at.

**`0x20` is not "the setting for a low modulation index".** It is the DAGC value
that pairs with `AfcLowBetaOn` in `RegAfcCtrl` (0x0B), and selecting it while
that bit is 0 sets half of a pair. Tried on the strength of the constant's name
after the modulation index fell to 1: reception went from 8-10 sync matches in 21
to **0 in 15**, and was reverted. The register comment already said `0x30` at
every setting this part runs at, and the name `_LOWBETA` was read over it. Also collapses the *rate* at which a
triggered RSSI measurement completes - by about 8000x - which is a second,
independent way to see whether it took effect.

**`RegRxBw` (0x19) `DccFreq`, bits 7:5, reset 0.** Sixteen times the recommended
corner. `RFM69_DCC_DEFAULT` is `0x80` (DccFreq 4).

### The bandwidth field of the same register, and how a rate change spent it

`rfm69_rx_bandwidth_to_reg()` picks **the narrowest encodable setting that is at
least what it is asked for**, so asking for the signal width gets a filter
exactly as wide as the signal and not one hertz wider. At 25 kbps the hub asked
for 100 kHz against a 75 kHz signal and carried 25 kHz of slack it never knew it
had. Doubling to 50 kbps made the signal 100 kHz and spent all of it in one
constant change, with nothing in the build disagreeing.

The signature is specific and worth recognising: **sync word matches, payload
fails CRC.** The preamble and the four sync bytes survive a filter that clips
sidebands; thirty-one bytes of payload do not. `sync 10, crc err 9, frames 1`
with `RegBitrate` and `RegFdev` reading back exactly correct is not a modem
mismatch and not a level problem — it is the channel filter.

Three things make it invisible until it fails:

- **`AfcAutoOn` was 0 here for the whole of that period**, so nothing corrected an
  off-centre carrier and `RegFei` read `0x0000` — the one measurement that would
  have sized the margin was the one the configuration disabled. It is on now, and
  the first thing it measured was 11230 Hz against a 12000 Hz allowance.
- The required width is `2 * (fdev + BR/2) + 2 * carrier_error`, and the carrier
  error term is usually absent from whatever assert guards it. Carson's rule
  alone is the zero-margin case.
- The device's value and the hub's are chosen independently. Here the WL55 side
  held `0x0Bu` (117.3 kHz) behind a comment reading "nearest step above the hub's
  100 kHz" — anchored to the hub's number at a bit rate that had since changed,
  and invisible to any assert on the hub's constant.

Encodable steps near this range, `FXOSC / (mant << (exp + 2))`: 83.3 (24,2),
**100.0 (20,2)**, **125.0 (16,2)**, 166.7 (24,1), 200.0 (20,1) kHz. Each step up
costs `10 log10` of the ratio in noise bandwidth — 100 → 125 kHz is ~0.97 dB.

**Read `0x19` back off the part after changing it.** `0x8a` is (20,2); `0x82` is
(16,2). The constant in the header is what was asked for, not what was encoded.

### The carrier sits kilohertz low, and a burst-length bias will inflate it

Measured off the air, not off the part. Two bursts in the same superframe on the
same channel are controls for each other: the beacon at t=0 goes out immediately
after the retune, the downlink at t=25 ms goes out on the same carrier with
25 ms more settling. Mean instantaneous frequency over each burst, seven
superframes on seven different channels:

```
burst              bytes   carrier      fdev   (fdev is 24963 off the part)
hub beacon   ch24     14   -21817      18830
hub downlink ch24     31    -7301      22916
DEV uplink   ch24     31     -944      23230
hub beacon   ch3      14   -23020      17424
hub downlink ch3      31   -12464      22546
DEV uplink   ch3      31    -1414      24104
```

**Only the matched-length rows may be compared.** The two 31-byte rows on each
channel are one board against another through the same estimator: this part sits
**7 to 12 kHz low** while the peer's SX126x is under 1.5 kHz. That is real and it
is the reason AFC matters here.

**The beacon rows look like a settling transient and cannot be read as one.** The
beacon is both the shortest burst *and* the earliest, so burst length and
settling time move together and no comparison of those two rows separates them.
The `fdev` column shows the bias directly - 25% low at 14 bytes, 8% at 31 - so
the extra 10 kHz at t=0 is at least partly the estimator. An apparent transient
was written into this file on that comparison and had to be withdrawn.

The peer's receiver was the witness that settled it: 8.65 kHz of filter slack and
**zero missed beacons on all 28 channels**, which a genuinely 22 kHz-low carrier
would not survive.

**Confounding is not a property of bad measurements.** Both benches produced one
in the same evening - a channel-versus-band list where the two were the same
split, and a settling-versus-length pair where the two moved together. Both were
plausible, both had controls, and neither control varied the thing that mattered.
Ask what *else* changes when the variable does, before quoting the number.

**Filter before you discriminate, or the number is quietly wrong.** The first
version of this measurement mixed to baseband and skipped the low-pass, so the
discriminator saw the whole 2.4 MHz capture. It returned a *plausible* carrier
(+6276 Hz of settling, sd 1998, consistent in sign across seven pairs) and was
out by more than a factor of two. What exposed it was asking the same code for
the deviation, where the same defect produced **255 kHz for a 25 kHz signal** -
absurd rather than merely wrong.

So: put a quantity with a known answer through every estimator. The carrier had
no such reference and absorbed the error silently; `RegFdev` read off the part
gave the deviation one, and that is the only reason the carrier figure was
caught. **An estimator with no control is a plausible-number generator.**

Even filtered, this deviation estimator under-reports and under-reports short
bursts worst - 18.8 kHz for a beacon whose register says 24 963. Use it to
compare two bursts, never to state a deviation.

Consequences that look like other faults:

- **A receive window opened shortly before an expected frame receives on a
  moving reference.** Sync matches on the eight bytes at the front and the
  payload behind it fails CRC, which reads exactly like a bandwidth or
  sensitivity problem and is neither. Widening `RegRxBw` buys margin against a
  static offset and nothing at all against a drifting one.
- **Low modulation index multiplies it.** At h = 2 the tones sit at ±fdev
  against half that bit rate; at h = 1 the same absolute drift eats twice the
  decision margin. The drift is the cause and h = 1 sets the threshold at which
  it starts costing frames.
- **A retune to the channel the part is already on restarts the transient.**
  Tuning "explicitly rather than inheriting" is defensible for correctness and
  expensive here, and the cost lands on whatever arrives next.

**Measure it with the SDR, never with the part**: the transmitter reads its own
`RegFrf` back correctly the whole time, because the register is right and the
oscillator is not. This is the `sdr` skill's pairing in its purest form.

Use the *mean* instantaneous frequency over the burst, not a spectrogram peak - a
peak lands on whichever FSK tone the data favoured and measures the modulation as
much as the carrier.

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

**The "it must be stale" reading has been proposed once and refuted.** Reading
the code alone says `rfm69_get_rssi()` triggers nothing, so the value must belong
to some earlier receive - and that argument was written into the roadmap before
anyone checked what a stale sample would have *read*. The part shows -92 dBm at
rest and the between-frames survey gives -86/-97; `rssi_up` came back at -25,
which is what a device a metre away should produce. The receiver's startup gates
its AGC and AFC phases behind RSSI crossing the threshold, so a restart parks
until a signal arrives rather than sampling the noise it restarted into. **The
line above already said this and was contradicted from first principles anyway.**

## The AGC never backs off, and the register nobody writes

**`RegLna` (0x18) is not written by `rfm69_init` or by anything else in the
driver.** The only path to it is `rfm69_set_lna_gain`, reachable from the console.
So the part runs on the reset value: `LnaGainSelect` 000, which nominally leaves
the AGC in charge, and `LnaZin` 0. Same class as `RegPaLevel` above — a field at
its reset value is a configuration, and it is one nobody chose.

`LnaCurrentGain`, bits 5:3, is **read-only** and reports the gain in force rather
than the one asked for. **Read it on `PayloadReady`, before the FIFO drain.**
`AutoRxRestartOn` restarts the receiver when the payload is read out, and the AGC
then re-derives on idle air and returns to G1, so a later read reports the idle
value under the frame's name. `afc_note()` reads it in the right place and its
comment says why.

Measured 2026-08-22, 120 frame rows, two devices, levels from −72 to −25 dBm:

    G1 (max gain)   118 rows,  including all six at -25 dBm
    G3                2 rows,  both at -50 dBm

**The AGC never once backed off**, including under the strongest signal this
bench has produced. G3 twice at −50 while G1 holds at −25 is not a gain schedule.

The reading is only worth having because the level column's own control passed in
the same data. A device-side reset restored a compiled-in +14 dBm transmit
default, so both boards jumped to maximum at their own first post-reset
transmission and the printed level moved with it — 23 dB asked for and 23
delivered on one board, 31 asked and 32 delivered on the other. **A gain with no
verified level beside it separates nothing**, which is the whole reason the
ROADMAP entry stayed open for weeks.

What this does **not** establish is that the loud end costs frames. Within one
board, 33 of 50 pass CRC at −48 dBm and 2 of 6 at −25: **p = 0.18**, n = 6.
The comparison that came back at p = 0.005 was the −45..−36 band against the −25
band, and on this bench those two bands are two different boards — level is
identical with board, so it says nothing about level. The solid result runs the
other way on the other board: 15 of 30 at −72 dBm against 31 of 34 at −40,
**p = 0.0003**. More signal helps, up to a turnover this data cannot locate.

## AFC: the one latch that is tied to its packet

`RegAfcFei` bit 2 is `AfcAutoOn` and bit 3 `AfcAutoclearOn`. With both set the
correction is measured at every receiver start-up and the previous one dropped
first, so `RegAfcValue` read after `PayloadReady` belongs to the packet just
received. That is the difference from RSSI and it is why the AFC read needs no
trigger of its own.

- **Read it before draining the FIFO.** `AutoRxRestartOn` re-arms the receiver
  when the payload is read out, and the next start-up overwrites the value.
- `RegAfcValue` is the correction *applied*; `RegFei` is what is left after it,
  and reads near zero while AFC is on. Reading `RegFei` to measure the error with
  AFC enabled returns the residual and looks like a clean carrier.
- **`AfcDone` sets on noise.** It proves the block runs, which is worth knowing
  once, and says nothing about any frame. A first non-zero value is evidence
  about the instrument before it is evidence about the carrier.
- Measured on this board: **11230 Hz** on the first sample, against a
  `RADIO_CARRIER_ERR_HZ` of 12000 that had never been measured. About 13 ppm at
  866 MHz, which is an ordinary crystal and not a fault in anyone's code.
- The sign convention has **not** been verified against an outside witness.

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
