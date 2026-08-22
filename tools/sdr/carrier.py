#!/usr/bin/env python3
"""Carrier offset between two transmitters, from one capture.

The dongle's own local oscillator is 30-150 ppm off and drifts while it warms,
which is a thousand times the offset worth measuring - so no absolute reading
from this instrument means anything. What does mean something is the difference
between two bursts in the *same* capture: the oscillator error is common to
both and cancels exactly.

The estimator is the midpoint of the two FSK tones, not the spectrum's centre
of mass. Centre of mass is pulled by whatever the data happened to be and by
how much of a short burst the detector caught, and it fails loudly here: a hub
beacon and a hub downlink 25 ms apart - one transmitter, two frame lengths -
read 18 kHz apart under centre of mass and agree to one bin under this.

The two tones sit at +/- fdev whatever is sent, so the midpoint is the carrier
and the data cannot move it.

**Ship the control with the number.** Two frames from one transmitter must
agree; the run prints that pair, and a spread over a bin or two means the
estimate is not to be read.
"""
import argparse
import sys

import numpy as np

import iqfile
import phy


def tone_midpoint(seg, rate, fdev, nfft=1 << 15):
    """Best-fitting two-tone centre: the shift where both tones are strongest."""
    n = min(len(seg), nfft)
    sp = np.abs(np.fft.fftshift(np.fft.fft(seg[:n] * np.hanning(n), nfft)))
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / rate))
    half = int(round(fdev / (freqs[1] - freqs[0])))
    pair = np.minimum(sp[:-2 * half], sp[2 * half:])
    k = int(np.argmax(pair)) + half
    return freqs[k], float(pair[k - half])


def channel_bursts(x, rate, centre, hz, fdev, min_len_us):
    """Everything on one channel, shifted to baseband and split into bursts."""
    n = np.arange(len(x), dtype=np.float64)
    y = x * np.exp(-2j * np.pi * (hz - centre) * n / rate).astype(np.complex64)
    y = iqfile.lowpass(y, rate, 90000)
    out = []
    for a, b in iqfile.find_bursts(y, rate, min_len_us=min_len_us):
        if b - a < 512:
            continue
        f0, power = tone_midpoint(y[a:b], rate, fdev)
        out.append({"t_ms": 1000.0 * a / rate, "air_ms": 1000.0 * (b - a) / rate,
                    "hz": f0, "power": power})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--channels", default="",
                    help="comma-separated grid channels; default every one in band")
    ap.add_argument("--min-us", type=float, default=1000.0)
    args = ap.parse_args()

    p = phy.constants()
    x, rate = iqfile.load(args.capture)
    meta = iqfile.read_meta(args.capture)
    centre = meta["centre"]
    fdev = p["RADIO_DEVIATION_HZ"]
    base, spacing = p["RADIO_CH_BASE_HZ"], p["RADIO_CH_SPACING_HZ"]

    if args.channels:
        chans = [int(c) for c in args.channels.split(",")]
    else:
        chans = [c for c in range(p["RADIO_GRID_COUNT"])
                 if abs(base + c * spacing - centre) < 0.4 * rate]

    print("resolution %.1f Hz/bin, fdev %d Hz, %d channel(s)"
          % (rate / float(1 << 15), fdev, len(chans)))
    print("%4s %10s %9s %12s %10s" % ("ch", "t (ms)", "air (ms)", "carrier Hz",
                                      "power"))
    for c in chans:
        rows = channel_bursts(x, rate, centre, base + c * spacing, fdev,
                              args.min_us)
        for r in rows:
            print("%4d %10.2f %9.2f %12.0f %10.0f"
                  % (c, r["t_ms"], r["air_ms"], r["hz"], r["power"]))
    print("""
Classify by position, never by size. An uplink opportunity repeats every
RADIO_SLOT_STRIDE * RADIO_SLOT_US - 611.0 ms at k=3 - and nothing the hub sends
does; the beacon is one superframe apart and the downlink two. Read the
difference between a burst on that 611 ms ladder and a hub burst in the same
superframe, and nothing else.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
