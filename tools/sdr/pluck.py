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
import sdrdev


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

    # Through the shared reader, so a 12-bit capture is not misread as bytes.
    raw, _ = iqfile.load_raw(a.src, meta)
    i0 = max(0, int((a.at - a.span / 2) / 1e3 * rate) * 2)
    i1 = min(len(raw), int((a.at + a.span / 2) / 1e3 * rate) * 2)
    if i1 - i0 < 1024:
        sys.exit("cut is empty - check --at against the capture length")

    x = raw[i0:i1]
    x = (x[0::2] + 1j * x[1::2]).astype(np.complex64)

    # The wanted channel to DC before the anti-alias filter, never after.
    # radio_devices_docs/open_hub/testing/sdr.md
    n = np.arange(len(x), dtype=np.float64)
    shift = a.freq - centre
    x *= np.exp(-2j * np.pi * shift * n / rate).astype(np.complex64)

    # Filter, then downsample: the other order destroys the kernel.
    # radio_devices_docs/open_hub/testing/sdr.md
    if a.decimate > 1:
        x = iqfile.lowpass(x, rate, rate / (2.0 * a.decimate) * 0.8, taps=257)
        x = x[::a.decimate]
        rate //= a.decimate

    # Written back in the source's format, not quantised down to 8 bits.
    f = sdrdev.fmt(meta["format"])
    scale = f["write_target"] / max(1e-9, float(np.abs(x).max()))
    out = np.empty(len(x) * 2, dtype=np.float32)
    out[0::2] = x.real * scale
    out[1::2] = x.imag * scale
    np.clip(out + f["centre"], f["code_min"], f["code_max"]) \
        .astype(np.dtype(f["dtype"])).tofile(a.dst)

    # offset=0: the channel is at DC now, so nothing downstream should shift it.
    iqfile.write_meta(a.dst, a.freq, rate, a.freq, 0, meta["format"],
                      backend=meta.get("backend"), serial=meta.get("serial"),
                      gain=meta.get("gain"))
    print(f"{a.dst}: {a.span:.0f} ms at {a.freq / 1e6:.3f} MHz, {rate / 1e3:.0f} kS/s")


if __name__ == "__main__":
    main()
