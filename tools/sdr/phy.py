#!/usr/bin/env python3
"""The PHY profile, read from the headers the firmware compiles.

A tool default that restates a firmware constant goes stale silently and turns
into a confident negative. radio_devices_docs/open_hub/testing/sdr.md
"""
import pathlib
import re

_INC = pathlib.Path(__file__).resolve().parents[2] / "Common" / "inc"

_WANTED = {
    "RADIO_BITRATE_BPS": "radio_slots.h",
    "RADIO_DEVIATION_HZ": "radio_phy.h",
    "RADIO_RX_BANDWIDTH_HZ": "radio_phy.h",
    "RADIO_CH_SPACING_HZ": "radio_phy.h",
}


def _scan(header, name):
    text = (_INC / header).read_text()
    m = re.search(r"^#define\s+%s\s+([0-9]+)u?\b" % re.escape(name), text, re.M)
    if m is None:
        raise KeyError("%s is not a plain integer define in %s" % (name, header))
    return int(m.group(1))


def profile():
    """Every constant, or an exception naming the one that moved."""
    return {n: _scan(h, n) for n, h in _WANTED.items()}


def bitrate():
    return _scan(_WANTED["RADIO_BITRATE_BPS"], "RADIO_BITRATE_BPS")


def deviation():
    return _scan(_WANTED["RADIO_DEVIATION_HZ"], "RADIO_DEVIATION_HZ")


def demod_cutoff():
    """Half the modulated bandwidth, which is what a complex low-pass wants."""
    return deviation() + bitrate() / 2.0


if __name__ == "__main__":
    for k, v in profile().items():
        print("%-24s %d" % (k, v))
