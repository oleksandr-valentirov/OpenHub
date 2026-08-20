# SDR test bench

An RTL-SDR on the host validates everything the hub transmits — carrier accuracy,
FSK deviation, bit rate, framing, burst timing, hop sequence and regulatory duty
cycle — without a second board.

It is **receive-only**. The hub's own receive path, pairing responses and ACKs
cannot be tested this way.

```bash
python3 -m venv .venv && .venv/bin/pip install numpy
cd tools/sdr
../../.venv/bin/python capture.py cap.iq -f 868e6 -t 5
../../.venv/bin/python decode.py cap.iq
../../.venv/bin/python dutycycle.py cap.iq     # exits 1 if over the ETSI limit
```

## Tools

| Tool | Purpose |
|---|---|
| `capture.py` | record IQ from the RTL-SDR |
| `iqfile.py` | read/write the capture format |
| `gfsk.py` | GFSK demodulation |
| `decode.py` | frame decoding — preamble, sync, payload, CRC |
| `dutycycle.py` | measure transmit duty cycle against the sub-band limit |
| `spectrum.py` | spectrum and spectrogram plots |
| `hops.py` | detect bursts and print the channel sequence |

`dutycycle.py` **exits non-zero** when the limit is exceeded, so it works as a gate
rather than a report. Given that the original firmware sat at 3.5% in a 1% band
([radio/phy.md](../radio/phy.md)), this is the check that matters most.

## What it has actually caught

- **The malformed broadcast frame.** `RFM_send_broadcast` offset its payload by
  `sizeof(header)` where `header` was a pointer. The frame decoded as
  `0d 00 00 00 ff 00 ...` with `hub_id` never assigned and `clock` truncated away.
  Invisible in review, unmistakable in a decode.
- **The 3.5% duty cycle**, from `DcFree=Manchester` doubling air time for nothing.
- **The join channel behaviour** — 0 bursts on 866.5 MHz with the pairing window
  closed, 8 bursts at exactly 4008 ms with it open
  ([radio/joining.md](../radio/joining.md)).
- **Its own duty-cycle blind spot**, above — caught by a second implementation
  using the same tools on different hardware.

## Traps in the tools themselves

Every one of these produced a **false negative** — a working feature reported as
broken. They are listed because the instinct on a null result is to suspect the
firmware.

- **`rtl_sdr -n` counts complex samples, not bytes.** Captures ran 2x long. Fixed
  in `capture.py`.
- **Locale.** With a Ukrainian locale `awk` truncated `866.4977` to `866`, because
  it expects a comma as the decimal separator. Prefix filter commands with
  `LC_ALL=C`.
- **Decimating the filter.** `np.convolve(x[::4], h[::4])` destroys the kernel.
  Filter at the full rate, *then* decimate.
- **Time-domain envelope detection does not work here.** A 75 kHz burst inside
  2.4 MHz of bandwidth is buried in noise. `hops.py` detects on a **spectrogram**
  with per-bin median noise floors, and bridges fragments (`--bridge-ms`) so one
  burst is not counted as several.
- **No detector is attributable on its own — including `hops.py`'s total.** The
  narrow one under-counts (one channel of 28); the wideband one over-counts,
  because it cannot tell this hub from a neighbour's 868 MHz doorbell or from the
  sensor-device bench beacon next door. `hops.py` maps bursts to grid channels,
  which helps, but its *total* counts them all too — quoting it as attributable
  was an overclaim of mine. Pass `--expect-ms 8.5` and it selects on air time and
  reports our share separately: 0.414% of a 0.713% band total, 10 bursts of 15.
- **Pick the filter that matches the transmitter.** `--expect-ms` identifies this
  hub because it hops across 28 channels on a fixed cadence, so air time plus
  cadence is a real signature. It is the wrong filter for a node parked on one
  channel: on the device side it credited a foreign 8.0 ms burst on another
  channel and inflated the figure by a quarter. `--channel N` is the right filter
  there, and the two combine.
- **The selection self-checks.** Whichever filter is used, the summary prints the
  cadence spread of what it selected. Irregular gaps read `SCATTERED` — a second
  transmitter got in, tighten the filter. Gaps that are whole multiples of the
  modal gap read `GAPS` — one of *our own* bursts was thrown away, loosen it.
  Those need opposite corrections, so they must not print the same thing.
- **The spectrogram quantises air time to whole slots.** `nfft / rate` — 0.853 ms
  at 2.4 Msps with the default 2048, and 2.0 ms on a 1.024 Msps capture, against
  an 8.5 ms frame. It is the right tool for *finding* and *attributing* bursts
  across a wide band and a poor one for *timing* a single burst. The summary
  prints the slot size, and the attributable total is re-measured on each burst's
  own narrowband envelope. A coarse slot inflated the device side's figure by 19%
  before this existed. Reduce it with a larger `--nfft` or a faster capture.
- **The envelope figure is a -6 dB width**, so it reads a couple of percent under
  the bit-count value (8.28 ms measured against 8.52 ms for 200 bits at 25 kbps).
  Worth knowing before treating the two as interchangeable.
- **A lowpass at the capture centre cannot measure a hopping transmitter**, and
  this one nearly published a wrong number. `dutycycle.py` used to filter around
  the centre frequency, so it saw one channel out of 28. On a real 20 s capture
  it reported **0.011%** where the true figure was **0.418%** — and printed a
  pass, from a tool documented as a gate. It now scans the whole band by
  default; `--narrow HZ` keeps the old behaviour for genuinely single-channel
  captures like the join channel.
- **The `-w` default was stale.** It was 15 kHz, left from the 9.6 kbps era. At
  25 kbps with 25 kHz deviation that cuts the FSK tones off: `decode.py` chops
  one burst into fragments and discards them, and `spectrum.py` reports a wrong
  deviation. Now 60 kHz. **This default has to move whenever the PHY does** —
  found by the device-side effort running these tools against its own radio.
- **The DC spike.** The RTL-SDR's centre-bin spike sets any naive global threshold.
  Per-bin floors handle it; a single global threshold does not.
- **Manchester decode phase.** The default is `phase=0`; `phase=1` yields plausible
  but wrong bytes rather than an obvious failure.

## Plucking one channel out of a wideband capture

`decode.py` thresholds against the peak of the whole file, so on a wideband
capture of a **hopping** transmitter the loudest thing in the band decides what
counts as a burst — and it is rarely the frame you want. `pluck.py` cuts one
channel and one moment out and re-emits them as a normal `.iq` + `.meta`:

```bash
.venv/bin/python pluck.py wide.iq one.iq --at 12836.7 --freq 865.7e6
.venv/bin/python decode.py one.iq --coding none -b 25000 -w 60e3
```

`decode.py --tune HZ` does the frequency shift without the cut, which is enough
when the capture is short.

**Run a control before believing a negative.** Push a frame you *know* decodes
through the same path. A bug in the analysis produces "sync not found", which is
indistinguishable from a transmitter that never keyed — and on this bench it has
now happened four times, every one of them in the tools. See
[radio/joining.md](../radio/joining.md#verification).

## Sample rate: the dongle has a gap, and nothing tells you

The RTL2832U produces **225–300 kS/s or 900 kS/s–3.2 MS/s** and nothing between.
A request in the gap is not an error `rtl_sdr` stops for — it prints
`Failed to set sample rate` and captures anyway, at whatever rate the hardware
was **last programmed with**, possibly by the other session. Everything measured
downstream is then scaled by an unknown factor.

`capture.py` refuses out-of-range rates and treats the warning as fatal: a
`.meta` claiming a rate the capture does not have is worse than no capture.

Note 900 000 is itself rejected — librtlsdr's range starts at 900 001. Use 1e6.

## Weak signals and the burst threshold

`find_bursts` takes its threshold from the **noise floor as well as the peak**,
whichever is higher. A fraction of the peak alone works only while the signal
dominates: when the strongest thing present is close to the floor, a quarter of
the peak lands *below* the noise, half the samples read as "on", and every burst
merges into one — which looks exactly like a transmitter that never keyed.

The hub reaches this dongle only ~12 dB above the noise, so this is the normal
case here, not an edge case.

For a signal that fragments anyway, `decode.py --bridge-ms` rejoins the pieces.
**A bridged burst's start and length are not measurements** — `find_sync` will
still locate the frame and the bytes are good, but read air time off an
unbridged capture.

## Sizing the capture band

`capture.py` tunes `--offset` (default 100 kHz) below the signal to dodge the
RTL-SDR's DC spike. An FSK signal is its deviation wide on **each** side of that,
so at the default 250 kS/s the upper tone of a ±25 kHz signal lands exactly on
the 125 kHz band edge: one tone survives, and the discriminator produces nothing.
`capture.py` warns when this is about to happen. Raise `--rate` or lower
`--offset`.

For the whole 865.1–867.9 MHz grid you need 2.8 MHz and the dongle gives 2.4,
so the outermost channels are missed. Budget for it: a beacon that falls on one
of them looks like a gap, which matters when the thing being measured **is** a
gap.

## See also

- [radio/phy.md](../radio/phy.md) — the settings being measured
- [radio/hopping.md](../radio/hopping.md) — reading the hop sequence off a waterfall
- `tools/sdr/README.md` — full command reference
