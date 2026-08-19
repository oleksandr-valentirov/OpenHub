#!/usr/bin/env python3
"""Demodulate a capture and print every RFM69 frame it contains.

Defaults match what the hub transmits today; override them as the PHY profile
changes.
"""
import argparse
import sys

import numpy as np

import gfsk
import iqfile


def describe(payload):
    """Best-effort read of the current broadcast layout, for eyeballing."""
    if len(payload) < 1:
        return ""
    return "len=%d addr=0x%02x rest=%s" % (
        payload[0], payload[1] if len(payload) > 1 else 0,
        payload[2:].hex(" ") or "-")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="raw u8 IQ capture from capture.py")
    ap.add_argument("-b", "--bitrate", type=float, default=32e6 / 0x0D05)
    ap.add_argument("-w", "--bandwidth", type=float, default=15e3,
                    help="demod low-pass cutoff in Hz")
    ap.add_argument("--sync", default="hell", help="sync word, ASCII or 0x-prefixed hex")
    ap.add_argument("--coding", choices=["none", "manchester", "whitening"],
                    default="manchester")
    a = ap.parse_args()

    sync = (bytes.fromhex(a.sync[2:]) if a.sync.startswith("0x")
            else a.sync.encode())

    x, rate = iqfile.load(a.path)
    bursts = iqfile.find_bursts(iqfile.lowpass(x, rate, a.bandwidth), rate)
    if not bursts:
        sys.exit("no bursts found - check frequency, gain and threshold")

    print(f"{len(bursts)} burst(s), bitrate {a.bitrate:.1f} bps, sync {sync.hex(' ')}")
    prev = None
    for i, (s, e) in enumerate(bursts):
        guard = int(rate * 1e-3)
        seg = x[max(0, s - guard):e + guard]
        inst = gfsk.discriminate(seg, rate, a.bandwidth)
        bitstr = gfsk.bits_to_string(gfsk.slice_bits(inst, rate, a.bitrate))
        gap = "" if prev is None else f"  gap {(s - prev) / rate * 1e3:8.2f} ms"
        print(f"\n[{i}] t={s / rate * 1e3:9.3f} ms  air={(e - s) / rate * 1e3:6.2f} ms{gap}")
        prev = s

        off = gfsk.find_sync(bitstr, sync)
        if off < 0:
            print("     sync not found")
            continue
        body = bitstr[off:]
        if a.coding == "manchester":
            body = gfsk.manchester_decode(body)
        data = gfsk.pack_bytes(body)
        if a.coding == "whitening":
            data = gfsk.dewhiten(data)
        print(f"     bytes: {data[:24].hex(' ')}")
        print(f"     {describe(data)}")

    total = sum(e - s for s, e in bursts) / rate
    span = len(x) / rate
    print(f"\nduty cycle over capture: {100 * total / span:.3f} %  "
          f"({total * 1e3:.1f} ms of {span:.2f} s)")


if __name__ == "__main__":
    main()
