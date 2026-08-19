#!/usr/bin/env python3
"""Measure transmit duty cycle against the ETSI EN 300 220 sub-band limits.

Compliance is per transmitter and per sub-band, so the check needs to know
which sub-band the carrier sits in.
"""
import argparse
import sys

import iqfile

# ERC/REC 70-03 sub-bands relevant to this project: (low Hz, high Hz, limit %, note)
SUBBANDS = [
    (863.0e6, 865.0e6, 0.1, "863-865"),
    (865.0e6, 868.0e6, 1.0, "865-868"),
    (868.0e6, 868.6e6, 1.0, "868.0-868.6"),
    (868.7e6, 869.2e6, 0.1, "868.7-869.2"),
    (869.4e6, 869.65e6, 10.0, "869.4-869.65"),
]


def limit_for(freq):
    for low, high, pct, name in SUBBANDS:
        if low <= freq < high:
            return pct, name
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("-w", "--bandwidth", type=float, default=15e3)
    a = ap.parse_args()

    meta = iqfile.read_meta(a.path)
    x, rate = iqfile.load(a.path)
    bursts = iqfile.find_bursts(iqfile.lowpass(x, rate, a.bandwidth), rate)
    span = len(x) / rate
    total = sum(e - s for s, e in bursts) / rate
    duty = 100 * total / span

    print(f"capture {span:.2f} s, {len(bursts)} burst(s), {total * 1e3:.1f} ms on air")
    if bursts:
        lens = [(e - s) / rate * 1e3 for s, e in bursts]
        print(f"burst length: min {min(lens):.2f} ms  max {max(lens):.2f} ms")
    print(f"duty cycle: {duty:.3f} %")

    freq = meta["signal"]
    if freq is None:
        print("no .meta - cannot pick a sub-band limit")
        return
    pct, name = limit_for(freq)
    if pct is None:
        print(f"{freq / 1e6:.3f} MHz is outside the sub-bands listed here")
        return
    verdict = "OK" if duty <= pct else "OVER LIMIT"
    print(f"sub-band {name} MHz allows {pct} % -> {verdict}")
    if duty > pct:
        sys.exit(1)


if __name__ == "__main__":
    main()
