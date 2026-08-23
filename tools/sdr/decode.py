#!/usr/bin/env python3
"""Demodulate a capture and print every RFM69 frame it contains.

Defaults match what the hub transmits today; override them as the PHY profile
changes.
"""
import argparse
import sys

import numpy as np

import frames
import gfsk
import iqfile
import phy


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="IQ capture from capture.py; format read from its .meta")
    # Read from Common/inc, so the PHY moves these and nobody has to remember.
    # radio_devices_docs/open_hub/testing/sdr.md
    ap.add_argument("-b", "--bitrate", type=float, default=phy.bitrate())
    ap.add_argument("-w", "--bandwidth", type=float, default=phy.demod_cutoff(),
                    help="demod low-pass cutoff in Hz")
    ap.add_argument("--sync", default="hell", help="sync word, ASCII or 0x-prefixed hex")
    # DcFree is 00 on the hub; this default moves with the PHY too.
    # radio_devices_docs/open_hub/testing/sdr.md
    ap.add_argument("--coding", choices=["none", "manchester", "whitening"],
                    default="none")
    ap.add_argument("--keys", default=None,
                    help="JSON of hex keys, so a sealed body can be read on the "
                         "bench: {\"session\": \"..16 bytes..\", "
                         "\"dev_id\": \"0xa5a5a5a5\"}. Debug only - the wire "
                         "carries no dev_id, so the slot owner must be named")
    ap.add_argument("--bridge-ms", type=float, default=0.0,
                    help="merge fragments closer than this. A weak signal breaks "
                         "one frame into several; bridging rejoins it, but the "
                         "resulting air times are not measurements")
    ap.add_argument("--tune", type=float, default=None,
                    help="absolute Hz to demodulate, for a wideband capture of a "
                         "hopping transmitter; without it the capture centre is used")
    a = ap.parse_args()

    sync = (bytes.fromhex(a.sync[2:]) if a.sync.startswith("0x")
            else a.sync.encode())
    keys = frames.load_keys(a.keys) if a.keys else {}
    if keys:
        print("keys loaded: %s" % ", ".join(sorted(keys)))

    x, rate = iqfile.load(a.path)

    # A hopping transmitter is never on the capture centre.
    # radio_devices_docs/open_hub/testing/sdr.md
    if a.tune is not None:
        meta = iqfile.read_meta(a.path)
        if meta["signal"] is None:
            sys.exit("--tune needs the .meta file to know what was captured")
        delta = a.tune - meta["signal"]
        n = np.arange(len(x), dtype=np.float64)
        x = (x * np.exp(-2j * np.pi * delta * n / rate)).astype(np.complex64)
        print(f"tuned {delta / 1e3:+.1f} kHz from the capture centre")

    bursts = iqfile.find_bursts(iqfile.lowpass(x, rate, a.bandwidth), rate,
                                bridge_us=a.bridge_ms * 1000.0)
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
        # data[0] is the RFM69 length byte; the frame proper starts after it.
        n = data[0] if data else 0
        print(f"     {frames.parse(data[1:1 + n], keys)}")

    total = sum(e - s for s, e in bursts) / rate
    span = len(x) / rate
    print(f"\nduty cycle over capture: {100 * total / span:.3f} %  "
          f"({total * 1e3:.1f} ms of {span:.2f} s)")


if __name__ == "__main__":
    main()
