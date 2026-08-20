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
    # Wideband by default. A lowpass around the capture centre only sees one
    # channel, and the hub hops across 28 of them - measured on a real capture,
    # the narrow path reported 0.011 % against a true 0.418 %, and passed.
    ap.add_argument("--narrow", type=float, metavar="HZ", default=None,
                    help="single-channel mode: lowpass at HZ instead of scanning "
                         "the whole capture. Only correct for a transmitter that "
                         "stays on the capture centre frequency.")
    ap.add_argument("--snr", type=float, default=15.0,
                    help="wideband threshold above the per-bin floor, dB")
    ap.add_argument("--min-ms", type=float, default=2.0,
                    help="discard bursts shorter than this")
    a = ap.parse_args()

    meta = iqfile.read_meta(a.path)
    x, rate = iqfile.load(a.path)
    span = len(x) / rate

    if a.narrow is not None:
        bursts = iqfile.find_bursts(iqfile.lowpass(x, rate, a.narrow), rate)
        total = sum(e - s for s, e in bursts) / rate
        mode = f"narrow, {a.narrow / 1e3:.0f} kHz lowpass"
    else:
        x = x - x.mean()               # kill the RTL-SDR centre spike
        wb, _, _, slot_s = iqfile.find_bursts_wideband(
            x, rate, snr_db=a.snr, min_ms=a.min_ms)
        bursts = [(int(s * slot_s * rate), int(e * slot_s * rate)) for s, e in wb]
        total = sum((e - s) for s, e in wb) * slot_s
        mode = "wideband"
    duty = 100 * total / span

    print(f"capture {span:.2f} s ({mode}), {len(bursts)} burst(s), "
          f"{total * 1e3:.1f} ms on air")
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
