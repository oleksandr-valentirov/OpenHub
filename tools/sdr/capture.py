#!/usr/bin/env python3
"""Capture IQ from an RTL-SDR into a raw u8 file that the other tools here read.

Tuning sits deliberately off-channel: the RTL-SDR has a large DC spike at its
centre frequency, which would swamp a signal parked exactly on it.
"""
import argparse
import shutil
import subprocess
import sys

DEFAULT_OFFSET = 100_000


def capture(path, freq, rate, seconds, gain, offset=DEFAULT_OFFSET):
    if shutil.which("rtl_sdr") is None:
        sys.exit("rtl_sdr not found - install rtl-sdr")
    centre = int(freq - offset)
    count = int(rate * seconds)  # rtl_sdr counts complex samples, not bytes
    cmd = ["rtl_sdr", "-f", str(centre), "-s", str(int(rate)),
           "-g", str(gain), "-n", str(count), path]
    print(" ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)
    meta = path + ".meta"
    with open(meta, "w") as fh:
        fh.write(f"centre={centre}\nrate={int(rate)}\nsignal={int(freq)}\noffset={int(offset)}\n")
    print(f"wrote {path} and {meta}", file=sys.stderr)
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="output .iq file")
    ap.add_argument("-f", "--freq", type=float, default=868e6, help="signal frequency in Hz")
    ap.add_argument("-s", "--rate", type=float, default=250e3, help="sample rate in Hz")
    ap.add_argument("-t", "--seconds", type=float, default=5.0)
    ap.add_argument("-g", "--gain", type=float, default=30)
    ap.add_argument("--offset", type=float, default=DEFAULT_OFFSET,
                    help="tune this far below the signal to dodge the DC spike")
    a = ap.parse_args()
    capture(a.path, a.freq, a.rate, a.seconds, a.gain, a.offset)


if __name__ == "__main__":
    main()
