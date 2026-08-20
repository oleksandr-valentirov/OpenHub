# PHY and channel plan

**Status: implemented and measured on air.**

## Modulation

| Parameter | Value |
|---|---|
| Modulation | GFSK |
| Bit rate | 25 kbps |
| Frequency deviation | 25 kHz |
| RX bandwidth | 100 kHz |
| DC-free coding | none |

25 kbps replaced the previous 9.6 kbps. The driver is the same either way; the
reason is the [duty-cycle budget](#duty-cycle) — a frame that takes 2.6x less air
time costs 2.6x less of the 1% allowance.

Deviation at 25 kHz gives a modulation index of 2, which is wide enough that
±20 ppm of crystal error at both ends still demodulates. Narrowing it to save
bandwidth would trade away exactly the margin cheap nodes need.

**Not Manchester.** `DcFree=Manchester` was the original setting and it is simply
wrong here: it doubles air time to achieve DC balance. That alone was a large
part of the old 3.5% duty cycle.

**Whitening is also off**, which is a change from the first fix. The SX1231 and
the device's SX126x use different whitening LFSR conventions, so making them
agree is interop work whose failure mode is silent. Unlike Manchester, whitening
costs no air time either way, so turning it off gives up nothing in the duty-cycle
budget, and the payload is AEAD ciphertext that is already DC balanced.

**The thing to watch:** the frame header is authenticated but *not* encrypted, so
it is plaintext, and fields like a small `device_id` can carry long runs of zero
bits. If the first hub-to-device tests show sync trouble rather than clean
frames, this is the first suspect.

The remedy to try **before** putting whitening back is cheaper and has no interop
cost: **assign `device_id` values from a range with mixed bits**. The ids are
ours to choose, so the runs can be removed by construction rather than by a
coding layer whose two implementations disagree. Re-enabling whitening stays the
fallback, not the first move.

Sync loss is unlikely to be total in any case — the device's preamble is 32 bits
against a 16-bit sync detector, so detection happens before the header arrives.
The exposure is bit-slicing inside the frame.

## Channel plan

| | |
|---|---|
| Base | 865.100 MHz |
| Spacing | 100 kHz |
| Grid slots | 29 (865.1 … 867.9 MHz) |
| Join slot | 14 → **866.5 MHz**, reserved |
| Hopping set | the other **28** channels |

The whole grid sits inside **865–868 MHz**, which is one regulatory sub-band with
one duty-cycle limit. This is the key constraint the old plan violated: it had 449
entries spanning four sub-bands with *different* limits, stepping 15.625 kHz —
narrower than the signal itself, so adjacent "channels" overlapped and the plan
was not really 449 channels at all.

Slot 14 is reserved so join traffic and data traffic cannot collide. The middle of
the band is chosen so the join channel is furthest from both sub-band edges,
where a device's crystal error has the most room before it pushes the signal out
of the allowed range. Details in [joining.md](joining.md).

## Duty cycle

**865–868 MHz allows 1% transmit duty cycle** per ERC REC 70-03 / ETSI EN 300 220.
This is the binding design constraint on the whole protocol — more binding than
throughput, power or range.

| | Duty cycle |
|---|---|
| Original firmware | ~3.5% — **over the limit** |
| Current, idle | **0.40%** — one 8.0 ms beacon per 2 s superframe |
| Downlink at half rate + join beacon | **0.91%** — the sustainable worst case |
| All three every superframe | **1.42% — over**, which is why both run at half rate |
| Join beacon, during a pairing window | +0.21% |
| Join beacon, window closed | 0 |

The reduction came from three things: whitening instead of Manchester, 25 kbps
instead of 9.6 kbps, and a frame that no longer carries a malformed oversized
payload.

`tools/sdr/dutycycle.py` measures this from an IQ capture and **exits non-zero if
the sub-band limit is exceeded**, so it can be used as a gate rather than read as
a report.

It scans the **whole capture band by default**, which is what a hopping
transmitter requires. Its previous lowpass-at-the-centre behaviour could only see
one channel: on a real 20 s capture it reported **0.011%** against a true ~0.42%
and printed a pass. See [testing/sdr.md](../testing/sdr.md).

**Neither detector is fully attributable, and the difference matters at 1%.**

| Method | 20 s capture | Bias |
|---|---|---|
| narrow lowpass at centre | 0.011% | **under** — sees 1 channel of 28 |
| wideband scan | 0.70% | **over** — counts every 868 MHz device nearby |
| `hops.py`, all bursts | 0.713% | **over**, for the same reason |
| `hops.py --expect-ms 8.5` | **0.414%** | attributable to the hub |

Air times are re-measured on each burst's envelope, because the spectrogram can
only report whole `nfft / rate` slots — 0.853 ms here. On this capture the two
agree to within a percent, but on a coarser capture the quantisation inflated the
device side's equivalent figure by 19%.

Selecting on air time is what separates our frames from everyone else's. In the
capture above, 10 of 15 bursts were 8.5 ms on a clean 2000 ms cadence — the hub —
and the other 5 were 4.3 to 15.4 ms at unrelated offsets: other traffic, or two
transmissions merged by fragment bridging. A bench beacon in the sensor-device
repository accounted for part of it.

Arithmetic agrees: one 8.5 ms frame per 2004 ms superframe is **0.424%**. A
measurement and a calculation from independent directions landing on the same
number is the reason to believe it.

Use the wideband gate as a **ceiling** — if it passes, the hub passes — and
`hops.py --expect-ms` when the question is what the hub itself is doing.

An in-firmware duty-cycle governor — the hub refusing to transmit rather than
trusting the schedule to stay within budget — is **planned, not built**. Until it
exists, any change to slot timing must be re-measured with the SDR.

## Frame format

`Common/inc/radio_protocol.h` holds the structures. Two frame types exist today:

| Type | Value | Channel | Encrypted |
|---|---|---|---|
| `RADIO_FRAME_DATA_BEACON` | `0x01` | hop sequence | planned |
| `RADIO_FRAME_JOIN_BEACON` | `0x02` | fixed join channel | never — see [joining.md](joining.md) |
| `RADIO_FRAME_PAIR_REQ` | `0x03` | fixed join channel | never — anonymous ECDH |
| `RADIO_FRAME_PAIR_RSP` | `0x04` | fixed join channel | never |
| `RADIO_FRAME_PAIR_CONF` | `0x05` | fixed join channel | never |
| `RADIO_FRAME_PAIR_ACCEPT` | `0x06` | fixed join channel | planned — sealed |

The data beacon is at **version 2**: 14 bytes, having grown `flags` and
`resume_in` for the [pairing quiesce](pairing.md). Its header said the layout was
frozen and that growth changes the version; it did. Only `PAIR_REQ` has a frozen
layout among the pairing frames — see [pairing.md](pairing.md#what-is-not-built).

**Every frame starts with its type byte.** The data beacon did not until a sweep
found it: it began with a broadcast address that filtered nothing, carried the
superframe counter in a field called `flags` and the hub's raw timer in one
called `clock`, and lived in a hub-private header a device cannot see. All three
were reasonable while it was a hub-only debug broadcast. See
[tdma.md](tdma.md#the-superframe-counter-is-the-shared-clock).

Packets use the RFM69 variable-length format with CRC on and hardware address
filtering available.

## A fixed bug worth remembering

`RFM_send_broadcast` offset its payload by `sizeof(header)` where `header` was a
**pointer** — 4 bytes instead of `sizeof(rfm_header_t)` = 1. The frame went out as
`0d 00 00 00 ff 00 ...`: `hub_id` never assigned, `clock` truncated away entirely.

It was found by decoding a capture off air, not by reading the code, which is the
argument for [the SDR bench](../testing/sdr.md) in one line.

## See also

- [hopping.md](hopping.md) — which of the 28 channels is used when
- [driver.md](driver.md) — how these settings reach the chip
- [ADR-0007](../decisions/0007-phy-profile.md)
