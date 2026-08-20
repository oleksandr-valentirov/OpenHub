#!/usr/bin/env python3
"""Cut one channel and one moment out of a wideband capture.

A hopping transmitter is never on the capture centre, and `decode.py`'s burst
detector thresholds against the peak of the whole file - so on a wideband
capture the loudest thing in the band decides what counts as a burst, and it is
rarely the frame you want. Plucking the channel out first makes it the only
thing in its own little capture.

Writes a normal .iq + .meta pair, so every other tool reads the result.
"""
import argparse
import sys

import numpy as np

import iqfile


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--at", type=float, required=True, help="centre of the cut, ms")
    ap.add_argument("--span", type=float, default=40.0, help="length of the cut, ms")
    ap.add_argument("--freq", type=float, required=True, help="channel to keep, Hz")
    ap.add_argument("--decimate", type=int, default=10)
    a = ap.parse_args()

    meta = iqfile.read_meta(a.src)
    if meta["signal"] is None:
        sys.exit("no .meta beside the source - cannot know what was captured")
    rate = meta["rate"]
    centre = meta["signal"] - meta["offset"]

    raw = np.fromfile(a.src, dtype=np.uint8)
    i0 = max(0, int((a.at - a.span / 2) / 1e3 * rate) * 2)
    i1 = min(len(raw), int((a.at + a.span / 2) / 1e3 * rate) * 2)
    if i1 - i0 < 1024:
        sys.exit("cut is empty - check --at against the capture length")

    x = raw[i0:i1].astype(np.float32) - 127.5
    x = (x[0::2] + 1j * x[1::2]).astype(np.complex64)

    # Bring the wanted channel to DC *before* the anti-alias filter. Putting it
    # anywhere else and then low-passing around DC deletes it - which looks
    # exactly like a transmitter that never keyed.
    n = np.arange(len(x), dtype=np.float64)
    shift = a.freq - centre
    x *= np.exp(-2j * np.pi * shift * n / rate).astype(np.complex64)

    # Filter, then downsample. The other order destroys the kernel; that mistake
    # has already cost this project one false negative, see README.
    if a.decimate > 1:
        x = iqfile.lowpass(x, rate, rate / (2.0 * a.decimate) * 0.8, taps=257)
        x = x[::a.decimate]
        rate //= a.decimate

    scale = 120.0 / max(1e-9, float(np.abs(x).max()))
    out = np.empty(len(x) * 2, dtype=np.float32)
    out[0::2] = x.real * scale
    out[1::2] = x.imag * scale
    np.clip(out + 127.5, 0, 255).astype(np.uint8).tofile(a.dst)

    # offset=0: the channel is at DC now, so nothing downstream should shift it.
    with open(a.dst + ".meta", "w") as fh:
        fh.write(f"centre={int(a.freq)}\nrate={int(rate)}\n"
                 f"signal={int(a.freq)}\noffset=0\n")
    print(f"{a.dst}: {a.span:.0f} ms at {a.freq / 1e6:.3f} MHz, {rate / 1e3:.0f} kS/s")


if __name__ == "__main__":
    main()
