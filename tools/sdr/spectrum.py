#!/usr/bin/env python3
"""Averaged spectrum of the transmit bursts - carrier offset and FSK deviation.

Averaging over the bursts rather than the whole capture keeps the idle noise
floor out of the estimate.
"""
import argparse

import numpy as np

import iqfile


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("-n", "--fft", type=int, default=2048)
    ap.add_argument("-w", "--bandwidth", type=float, default=60e3)
    ap.add_argument("--rows", type=int, default=28, help="height of the ASCII plot")
    a = ap.parse_args()

    meta = iqfile.read_meta(a.path)
    x, rate = iqfile.load(a.path)
    bursts = iqfile.find_bursts(iqfile.lowpass(x, rate, a.bandwidth), rate)
    if not bursts:
        raise SystemExit("no bursts found")

    n = a.fft
    acc = np.zeros(n)
    count = 0
    win = np.hanning(n)
    for s, e in bursts:
        for i in range(s, e - n, n // 2):
            acc += np.abs(np.fft.fftshift(np.fft.fft(x[i:i + n] * win))) ** 2
            count += 1
    spec = 10 * np.log10(acc / max(count, 1) + 1e-9)
    spec -= spec.max()
    freqs = np.fft.fftshift(np.fft.fftfreq(n, 1 / rate))

    peak = freqs[int(np.argmax(spec))]
    print(f"centre tuned {meta['centre']} Hz, signal expected at {meta['signal']} Hz")
    print(f"strongest bin {peak:+.0f} Hz from nominal")

    # the two FSK tones sit either side of the carrier
    left = freqs[freqs < -500]
    right = freqs[freqs > 500]
    if len(left) and len(right):
        lo = left[int(np.argmax(spec[freqs < -500]))]
        hi = right[int(np.argmax(spec[freqs > 500]))]
        print(f"FSK tones {lo:+.0f} / {hi:+.0f} Hz -> deviation ~{(hi - lo) / 2 / 1e3:.1f} kHz")

    mask = np.abs(freqs) < 40e3
    fs, ss = freqs[mask], np.clip(spec[mask], -50, 0)
    cols = 76
    edges = np.linspace(0, len(fs), cols + 1).astype(int)
    binned = [ss[edges[i]:max(edges[i + 1], edges[i] + 1)].max() for i in range(cols)]
    for row in range(a.rows):
        level = -row * 50 / a.rows
        line = "".join("#" if v >= level else " " for v in binned)
        label = f"{level:5.0f}" if row % 4 == 0 else "     "
        print(f"{label} |{line}")
    print(f"      +{'-' * cols}")
    print(f"       {fs[0] / 1e3:.0f} kHz{' ' * (cols - 18)}{fs[-1] / 1e3:+.0f} kHz")


if __name__ == "__main__":
    main()
