#!/usr/bin/env python3
"""Transmit a hub-legal frame from the bladeRF, at a carrier offset chosen by the caller.

This is the transmit half the RTL-SDR never had. It exists to stimulate the
hub's *receive* path, which no firmware counter can separate from the device's
transmit path: "the device stopped sending" and "the hub stopped hearing"
produce the same zero, and until now only one side of that could be driven.

Every framing constant comes from phy.py, which reads the headers the firmware
compiles - including the sync word, which a transmitter that restated it could
be wrong about silently.

The local oscillator is deliberately not the target frequency. Both radios here
are zero-IF and leak an unmodulated carrier at the LO; tuned on channel that
leak would sit in the middle of the hub's filter and be the strongest thing in
it. The signal is placed at a baseband offset instead and the LO moved down by
the same amount, so the leak lands outside the channel being measured.
radio_devices_docs/open_hub/testing/sdr.md
"""
import argparse
import contextlib
import fcntl
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

import gfsk
import iqfile
import phy
import sdrdev

# Far enough that the LO leak clears the widest filter the part can encode.
LO_GAP_HZ = 400_000


@contextlib.contextmanager
def sdr_lock(path, label, wait_s):
    """Hold the transmitter for the run, or name who has it."""
    fh = open(path, "a+")
    deadline = time.time() + wait_s
    while True:
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if time.time() >= deadline:
                fh.seek(0)
                raise SystemExit("bladeRF held by: %s" % (fh.read().strip() or "?"))
            time.sleep(0.5)
    fh.seek(0)
    fh.truncate()
    fh.write("%s pid %d\n" % (label, os.getpid()))
    fh.flush()
    try:
        yield
    finally:
        fh.seek(0)
        fh.truncate()
        fh.flush()
        fcntl.flock(fh, fcntl.LOCK_UN)
        fh.close()


def build_frame(payload, crc_payload_only=False):
    """The bytes an RFM69 puts on air: preamble, sync, length, payload, CRC.

    The CRC covers the length byte, which was measured on the part rather than
    read: over the payload alone the hub matched sync on every frame and passed
    none of them, and 23 of 28 passed once the length byte was included.
    radio_devices_docs/open_hub/radio/configuration.md
    """
    c = phy.constants()
    body = bytes([len(payload)]) + payload
    crc = gfsk.crc16_ccitt(payload if crc_payload_only else body,
                           init=c["RADIO_CRC_SEED"])
    # RADIO_CRC_INVERTED, and the part sends the CRC most significant byte first.
    crc ^= 0xFFFF
    return (b"\xaa" * c["RADIO_PREAMBLE_BYTES"] + phy.sync_word() + body
            + bytes([(crc >> 8) & 0xFF, crc & 0xFF]))


def bits_of(data):
    """Most significant bit first, which is the order the part shifts them out."""
    return np.unpackbits(np.frombuffer(data, dtype=np.uint8))


def gaussian_taps(bt, sps, span=4):
    """Gaussian pulse, normalised to unit area so the deviation is the deviation."""
    t = np.arange(-span * sps, span * sps + 1, dtype=np.float64) / sps
    sigma = np.sqrt(np.log(2.0)) / (2.0 * np.pi * bt)
    h = np.exp(-(t ** 2) / (2.0 * sigma ** 2))
    return h / h.sum()


def modulate(frame, rate, offset_hz, pad_ms=2.0):
    """GFSK the frame at the PHY's own bitrate, deviation and shaping."""
    c = phy.constants()
    sps = rate / c["RADIO_BITRATE_BPS"]
    nrz = np.repeat(bits_of(frame).astype(np.float64) * 2.0 - 1.0, int(round(sps)))
    shaped = np.convolve(nrz, gaussian_taps(phy.shaping_bt(), int(round(sps))), "same")
    # Phase is the integral of frequency; the deviation is per symbol, not peak-to-peak.
    phase = np.cumsum(2.0 * np.pi * c["RADIO_DEVIATION_HZ"] * shaped / rate)
    n = np.arange(len(phase), dtype=np.float64)
    iq = np.exp(1j * (phase + 2.0 * np.pi * offset_hz * n / rate))
    # Silence either side, so the file has the edges every burst detector needs.
    pad = np.zeros(int(rate * pad_ms / 1e3), dtype=complex)
    return np.concatenate([pad, iq, pad])


def to_sc16q11(iq, scale):
    """Interleaved signed 16-bit carrying 12 bits, which is what the bladeRF takes."""
    full = sdrdev.fmt("sc16q11")["fullscale"] * scale
    out = np.empty(2 * len(iq), dtype=np.int16)
    out[0::2] = np.round(np.real(iq) * full)
    out[1::2] = np.round(np.imag(iq) * full)
    return out


def transmit(path, lo_hz, rate, gain, repeat, gap_us, serial=None):
    """Hand the file to bladeRF-cli, which repeats it with a gap of its own."""
    script = [
        "set frequency tx %d" % int(lo_hz),
        "set samplerate tx %d" % int(rate),
        "set bandwidth tx %d" % int(rate),
        "set gain tx %d" % int(gain),
        "tx config file=%s format=bin repeat=%d delay=%d" % (path, repeat, gap_us),
        "tx start",
        "tx wait",
    ]
    cmd = ["bladeRF-cli"]
    if serial:
        cmd += ["-d", "*:serial=%s" % serial]
    for line in script:
        cmd += ["-e", line]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-f", "--freq", type=float, default=None,
                    help="target carrier; default is the join channel")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="carrier error to add, in Hz - the swept axis")
    ap.add_argument("-p", "--payload", default="dummy_payload")
    ap.add_argument("-n", "--repeat", type=int, default=200)
    ap.add_argument("--gap-us", type=int, default=20000)
    ap.add_argument("-s", "--rate", type=float, default=2e6)
    ap.add_argument("-g", "--gain", type=int, default=20)
    ap.add_argument("--scale", type=float, default=0.7,
                    help="fraction of full scale, kept off the rail by default")
    ap.add_argument("--crc-payload-only", action="store_true",
                    help="leave the length byte out of the CRC, which the part refuses")
    ap.add_argument("--pad-ms", type=float, default=2.0)
    ap.add_argument("--label", default="hub")
    ap.add_argument("--wait", type=float, default=60.0)
    ap.add_argument("--dry-run", metavar="FILE",
                    help="write the waveform and stop, for decode.py to check")
    a = ap.parse_args()

    c = phy.constants()
    target = a.freq if a.freq else phy.channel_hz(c["RADIO_JOIN_SLOT"])
    lo = target - LO_GAP_HZ
    baseband = LO_GAP_HZ + a.offset

    frame = build_frame(a.payload.encode(), a.crc_payload_only)
    iq = modulate(frame, a.rate, baseband, a.pad_ms)
    samples = to_sc16q11(iq, a.scale)

    print("frame   %d bytes on air, payload %r" % (len(frame), a.payload))
    print("        %s" % frame.hex(" "))
    print("carrier %.6f MHz target, %+.0f Hz offset" % (target / 1e6, a.offset))
    print("        LO %.6f MHz, signal at %+.0f Hz baseband" % (lo / 1e6, baseband))
    print("air     %.2f ms per frame, %d frames, %.0f ms gap"
          % (1e3 * len(iq) / a.rate, a.repeat, a.gap_us / 1e3))

    if a.dry_run:
        samples.tofile(a.dry_run)
        iqfile.write_meta(a.dry_run, lo, a.rate, target, LO_GAP_HZ, "sc16q11",
                          backend="bladerf")
        print("wrote   %s (not transmitted)" % a.dry_run)
        return 0

    devs = sdrdev.by_key("bladerf").devices()
    if not devs:
        raise SystemExit("no bladeRF on the bus; sdrinfo.py says what is")
    serial = devs[0].get("serial")

    with sdr_lock("/tmp/openhub-bladerf.lock", a.label, a.wait):
        loaded = sdrdev.by_key("bladerf").ensure_tx_fpga(serial)
        if loaded:
            print("fpga    reloaded from %s - the flash image does not transmit"
                  % loaded)
        with tempfile.NamedTemporaryFile(suffix=".sc16", delete=False) as fh:
            samples.tofile(fh)
            path = fh.name
        try:
            r = transmit(path, lo, a.rate, a.gain, a.repeat, a.gap_us, serial)
        finally:
            os.unlink(path)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("bladeRF-cli failed")
    print("sent    %d frames" % a.repeat)
    return 0


if __name__ == "__main__":
    sys.exit(main())
