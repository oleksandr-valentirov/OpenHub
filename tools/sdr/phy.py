#!/usr/bin/env python3
"""The PHY profile and the slot geometry, read from the headers the firmware compiles.

A tool default that restates a firmware constant goes stale silently and turns
into a confident negative. radio_devices_docs/open_hub/testing/sdr.md

The values are read through the C preprocessor rather than by matching text.
Half the geometry is an expression over other macros - RADIO_SLOT_US is
RADIO_UPLINK_AIR_US + RADIO_SLOT_GUARD_US - and a regular expression cannot
evaluate one, so a scanner either restates it or silently has no answer. The
compiler is the same one the firmware uses, which is the point: there is one
definition and no second implementation of it to drift.
"""
import functools
import pathlib
import subprocess
import sys
import tempfile

_ROOT = pathlib.Path(__file__).resolve().parents[2]
_INC = _ROOT / "Common" / "inc"

_HEADERS = ("radio_phy.h", "radio_slots.h", "radio_protocol.h")

_NAMES = (
    "RADIO_BITRATE_BPS", "RADIO_DEVIATION_HZ", "RADIO_RX_BANDWIDTH_HZ",
    "RADIO_CH_BASE_HZ", "RADIO_CH_SPACING_HZ", "RADIO_GRID_COUNT",
    "RADIO_JOIN_SLOT", "SUPERFRAME_US", "RADIO_SLOT_US", "RADIO_SLOT_GUARD_US",
    "RADIO_BEACON_OFFSET_US", "RADIO_BEACON_LEN_US",
    "RADIO_DOWNLINK_OFFSET_US", "RADIO_DOWNLINK_BYTES",
    "RADIO_UPLINK_OFFSET_US", "RADIO_UPLINK_BYTES", "RADIO_UPLINK_AIR_US",
    "RADIO_SLOT_COUNT", "RADIO_SLOT_STRIDE", "RADIO_SLOT_OPPS",
    "RADIO_US_PER_BYTE", "RADIO_FRAME_OVERHEAD_B", "RADIO_AIR_START_TO_SYNC_US",
)

# No macro carries the beacon's length, so it comes off the wire struct.
_SIZEOF = ("radio_data_beacon_t", "radio_uplink_t", "radio_downlink_t")


@functools.lru_cache(maxsize=1)
def constants():
    """Every name in _NAMES, or an exception naming the one that moved."""
    src = ["#include <stdint.h>", "#include <stdio.h>"]
    src += ['#include "%s"' % h for h in _HEADERS]
    src.append("int main(void) {")
    for n in _NAMES:
        src.append('    printf("%s=%%lld\\n", (long long)(%s));' % (n, n))
    for s in _SIZEOF:
        src.append('    printf("sizeof %s=%%lld\\n", (long long)sizeof(%s));' % (s, s))
    src += ["    return 0;", "}"]

    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / "dump.c"
        exe = pathlib.Path(d) / "dump"
        c.write_text("\n".join(src) + "\n")
        cc = subprocess.run(["cc", "-std=c11", "-I", str(_INC), "-o", str(exe), str(c)],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            raise RuntimeError("the headers did not compile; a name below has moved "
                               "or changed shape:\n" + cc.stderr)
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    return {k: int(v) for k, v in
            (ln.split("=", 1) for ln in out.stdout.splitlines() if "=" in ln)}


def profile():
    """The four modem constants, kept for the tools that only want those."""
    c = constants()
    return {n: c[n] for n in ("RADIO_BITRATE_BPS", "RADIO_DEVIATION_HZ",
                              "RADIO_RX_BANDWIDTH_HZ", "RADIO_CH_SPACING_HZ")}


def bitrate():
    return constants()["RADIO_BITRATE_BPS"]


def deviation():
    return constants()["RADIO_DEVIATION_HZ"]


def demod_cutoff():
    """Half the modulated bandwidth, which is what a complex low-pass wants."""
    return deviation() + bitrate() / 2.0


def channel_hz(n):
    c = constants()
    return c["RADIO_CH_BASE_HZ"] + n * c["RADIO_CH_SPACING_HZ"]


def air_us(payload_b):
    """RADIO_AIR_START_TO_END_US, which is a macro and so is recomputed here."""
    c = constants()
    return (payload_b + c["RADIO_FRAME_OVERHEAD_B"]) * c["RADIO_US_PER_BYTE"]


def slot_offset_us(n):
    c = constants()
    return c["RADIO_UPLINK_OFFSET_US"] + n * c["RADIO_SLOT_US"]


def uplink_slots(device):
    """The RADIO_SLOT_OPPS slots one device owns, at RADIO_SLOT_STRIDE apart."""
    c = constants()
    return [device + k * c["RADIO_SLOT_STRIDE"] for k in range(c["RADIO_SLOT_OPPS"])]


if __name__ == "__main__":
    for k, v in constants().items():
        print("%-28s %d" % (k, v))
    sys.stderr.write("\nread through cc from %s\n" % _INC)
