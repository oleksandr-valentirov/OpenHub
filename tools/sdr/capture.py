#!/usr/bin/env python3
"""Capture IQ from an RTL-SDR into a raw u8 file that the other tools here read.

Tuning sits deliberately off-channel: the RTL-SDR has a large DC spike at its
centre frequency, which would swamp a signal parked exactly on it.
"""
import argparse
import contextlib
import fcntl
import os
import shutil
import subprocess
import sys
import time

import phy

# Far enough from the DC spike, close enough that both FSK tones still fit.
# radio_devices_docs/open_hub/testing/sdr.md
DEFAULT_OFFSET = 60_000

# The only rates the RTL2832U produces; one in the gap captures anyway.
# radio_devices_docs/open_hub/testing/sdr.md
RATE_RANGES = ((225_001, 300_000), (900_001, 3_200_000))


def check_rate(rate):
    rate = int(rate)
    if any(lo <= rate <= hi for lo, hi in RATE_RANGES):
        return rate
    ranges = " or ".join(f"{lo}-{hi}" for lo, hi in RATE_RANGES)
    sys.exit(f"sample rate {rate} is not one the RTL2832U can produce ({ranges} Hz).\n"
             f"rtl_sdr would warn and capture at the wrong rate instead of stopping.")

# One dongle, claimed exclusively, and two sessions that both reach for it.
# radio_devices_docs/open_hub/testing/sdr.md
LOCK_PATH = os.environ.get("OPENHUB_SDR_LOCK", "/tmp/openhub-rtlsdr.lock")


@contextlib.contextmanager
def sdr_lock(label, wait_s):
    """Hold the dongle for the duration of a capture, or say who has it."""
    fh = open(LOCK_PATH, "a+")
    deadline = time.time() + wait_s
    while True:
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if time.time() >= deadline:
                fh.seek(0)
                holder = fh.read().strip() or "another process"
                fh.close()
                sys.exit(f"RTL-SDR busy: {holder}\n"
                         f"Wait, or raise --wait. Lock: {LOCK_PATH}")
            time.sleep(0.5)

    fh.seek(0)
    fh.truncate()
    fh.write(f"{label}, pid {os.getpid()}, since {time.strftime('%H:%M:%S')}\n")
    fh.flush()
    try:
        yield
    finally:
        fh.seek(0)
        fh.truncate()
        fh.flush()
        fcntl.flock(fh, fcntl.LOCK_UN)
        fh.close()


def capture(path, freq, rate, seconds, gain, offset=DEFAULT_OFFSET,
            label="capture", wait_s=0.0):
    if shutil.which("rtl_sdr") is None:
        sys.exit("rtl_sdr not found - install rtl-sdr")
    centre = int(freq - offset)
    rate = check_rate(rate)
    count = int(rate * seconds)  # rtl_sdr counts complex samples, not bytes
    cmd = ["rtl_sdr", "-f", str(centre), "-s", str(int(rate)),
           "-g", str(gain), "-n", str(count), path]
    print(" ".join(cmd), file=sys.stderr)
    with sdr_lock(label, wait_s):
        proc = subprocess.run(cmd, stderr=subprocess.PIPE)
    err = proc.stderr.decode(errors="replace")
    # Written before the returncode is judged, so the explanation survives.
    # radio_devices_docs/open_hub/testing/sdr.md
    sys.stderr.write(err)
    if proc.returncode != 0:
        sys.exit(f"rtl_sdr exited {proc.returncode}")
    # Fatal, not printed: a meta file claiming a wrong rate outlives the capture.
    # radio_devices_docs/open_hub/testing/sdr.md
    if "Failed to set sample rate" in err:
        sys.exit("rtl_sdr could not set the sample rate; the capture's time axis "
                 "would be wrong. Refusing to write a .meta that claims otherwise.")
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
    ap.add_argument("--label", default=os.environ.get("OPENHUB_SDR_LABEL", "capture"),
                    help="who is holding the dongle, shown to whoever is blocked")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="seconds to wait for the dongle before giving up")
    a = ap.parse_args()

    # An offset past the Nyquist edge clips one tone and reads as silence.
    # radio_devices_docs/open_hub/testing/sdr.md
    edge = a.rate / 2.0
    if a.offset + phy.demod_cutoff() >= edge * 0.95:
        print(f"warning: signal at +{a.offset/1e3:.0f} kHz is within 5% of the "
              f"{edge/1e3:.0f} kHz band edge at {a.rate/1e3:.0f} kS/s.\n"
              f"         An FSK tone will be clipped. Raise --rate, or lower "
              f"--offset.", file=sys.stderr)
    capture(a.path, a.freq, a.rate, a.seconds, a.gain, a.offset, a.label, a.wait)


if __name__ == "__main__":
    main()
