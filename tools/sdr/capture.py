#!/usr/bin/env python3
"""Capture IQ from whichever SDR is on the bench, into a file the other tools read.

The receiver is chosen by asking `sdrdev` what is plugged in, not by assuming.
Which one was used, at what gain, in what sample format, is written into the
`.meta` beside the capture - so a file that outlives the session still names
the instrument it came off.

Tuning sits deliberately off-channel. Both receivers here are zero-IF and both
put a DC spike at their centre frequency, which would otherwise sit on top of
the carrier. The offset is recorded and undone by the other tools.
"""
import argparse
import contextlib
import fcntl
import os
import subprocess
import sys
import time

import iqfile
import phy
import sdrdev


@contextlib.contextmanager
def sdr_lock(path, label, wait_s, what):
    """Hold one receiver for the duration of a capture, or say who has it.

    The lock is per receiver, not per bench, and the RTL-SDR's path is unchanged
    from when it was the only one. radio_devices_docs/open_hub/testing/sdr.md
    """
    fh = open(path, "a+")
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
                sys.exit(f"{what} busy: {holder}\n"
                         f"Wait, or raise --wait. Lock: {path}")
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


# The capture's time axis is not what the .meta would claim. Fatal, not printed.
# radio_devices_docs/open_hub/testing/sdr.md
_RATE_LIES = ("Failed to set sample rate", "Failed to set samplerate",
              "unable to set samplerate")
# A bladeRF 1 with no FPGA image enumerates, opens, and reads as a dead antenna.
# radio_devices_docs/open_hub/testing/sdr.md
_NOT_READY = ("FPGA is not configured", "Operation timed out",
              "No devices available")


def capture(path, freq, rate, seconds, gain, offset=None, label="capture",
            wait_s=0.0, prefer=None, bandwidth=None):
    backend, devices = sdrdev.select(prefer)
    serial = devices[0]["serial"] if devices else None
    model = devices[0]["model"] if devices else backend.label
    if offset is None:
        offset = backend.default_offset

    rate, why = backend.check_rate(rate)
    if why:
        sys.exit(why + "\nA rate the part cannot produce is not refused by every "
                       "driver - some warn and capture at another rate, and the "
                       ".meta then claims a time axis the file does not have.")
    bad = backend.check_freq(freq - offset)
    if bad:
        sys.exit(bad)

    centre = int(freq - offset)
    print(f"{model} (sn={serial or 'n/a'}): {rate/1e6:.3f} Msps, "
          f"centre {centre/1e6:.3f} MHz, {seconds:g} s, gain {gain}, "
          f"format {backend.sample_format}", file=sys.stderr)

    cmds = backend.capture_cmd(path, centre, rate, seconds, gain,
                               serial=serial, bandwidth=bandwidth)
    with sdr_lock(backend.lock_path(), label, wait_s, backend.label):
        for cmd in cmds:
            print(" ".join(cmd), file=sys.stderr)
            proc = subprocess.run(cmd, stderr=subprocess.PIPE)
            err = proc.stderr.decode(errors="replace")
            # Written before the returncode is judged, so it survives.
            # radio_devices_docs/open_hub/testing/sdr.md
            sys.stderr.write(err)
            if proc.returncode != 0:
                hint = ""
                if any(p in err for p in _NOT_READY):
                    hint = (f"\n{backend.label} enumerated but would not stream. "
                            f"On a bladeRF 1 the FPGA image is loaded at every "
                            f"power-up: install bladerf-fpga-hostedx40 (or "
                            f"-hostedx115) and check `bladeRF-cli -e info`.")
                sys.exit(f"{cmd[0]} exited {proc.returncode}{hint}")
            if any(p in err for p in _RATE_LIES):
                sys.exit(f"{backend.label} could not set the sample rate; the "
                         f"capture's time axis would be wrong. Refusing to write "
                         f"a .meta that claims otherwise.")

    meta = iqfile.write_meta(path, centre, rate, freq, offset,
                             backend.sample_format, backend=backend.key,
                             serial=serial, gain=gain,
                             bandwidth=backend.effective_bandwidth(rate, bandwidth))
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
    ap.add_argument("-d", "--device", default="auto",
                    choices=["auto"] + [b.key for b in sdrdev.BACKENDS],
                    help="which receiver to use; auto picks the widest ready one")
    ap.add_argument("--bandwidth", type=float, default=None,
                    help="analog filter width, Hz (bladeRF only; defaults to the "
                         "sample rate so the whole captured span is passed)")
    ap.add_argument("--offset", type=float, default=None,
                    help="tune this far below the signal to dodge the DC spike")
    ap.add_argument("--label", default=os.environ.get("OPENHUB_SDR_LABEL", "capture"),
                    help="who is holding the receiver, shown to whoever is blocked")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="seconds to wait for the receiver before giving up")
    a = ap.parse_args()

    # An offset past the Nyquist edge clips one tone and reads as silence.
    # radio_devices_docs/open_hub/testing/sdr.md
    offset = a.offset
    if offset is None:
        try:
            offset = sdrdev.select(a.device)[0].default_offset
        except SystemExit:
            offset = sdrdev.RtlSdr.default_offset
    edge = a.rate / 2.0
    if offset + phy.demod_cutoff() >= edge * 0.95:
        print(f"warning: signal at +{offset/1e3:.0f} kHz is within 5% of the "
              f"{edge/1e3:.0f} kHz band edge at {a.rate/1e3:.0f} kS/s.\n"
              f"         An FSK tone will be clipped. Raise --rate, or lower "
              f"--offset.", file=sys.stderr)
    capture(a.path, a.freq, a.rate, a.seconds, a.gain, a.offset, a.label,
            a.wait, prefer=a.device, bandwidth=a.bandwidth)


if __name__ == "__main__":
    main()
