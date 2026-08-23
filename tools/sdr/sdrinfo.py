#!/usr/bin/env python3
"""What radio is on the bench right now, and which checks it can actually run.

Run this before believing any capture, and first of all after plugging
something in or unplugging it. The two receivers on this bench do not answer
the same questions: the RTL-SDR's usable span stops four channels short of the
hopping grid, and it cannot transmit at all, so a whole class of check is
simply unavailable on it. Which class is not a thing to remember - it is
printed below, derived from the part and from `phy.py`.

Exit status is 0 when at least one receiver can be driven, 1 when none can, so
a script can gate on it.
"""
import argparse
import sys

import sdrdev

# What each tool needs. Narrower than span_hz does not fail, it under-reports.
# radio_devices_docs/open_hub/testing/sdr.md
TOOLS = (
    {"name": "phy.py", "needs": (), "span_hz": 0,
     "what": "print the PHY profile the tools will use"},
    {"name": "capture.py", "needs": ("rx",), "span_hz": 0,
     "what": "record IQ from whichever receiver is present"},
    {"name": "spectrum.py", "needs": ("rx",), "span_hz": 0,
     "what": "carrier offset and FSK deviation of a burst"},
    {"name": "decode.py", "needs": ("rx",), "span_hz": 0,
     "what": "frames, byte by byte, sealed bodies with operator keys"},
    {"name": "dutycycle.py", "needs": ("rx",), "span_hz": 0,
     "what": "ETSI EN 300 220 sub-band duty cycle"},
    {"name": "pluck.py", "needs": (), "span_hz": 0,
     "what": "cut one channel out of a wideband capture (offline)"},
    {"name": "hops.py", "needs": ("rx",), "span_hz": "grid",
     "what": "follow the transmitter across the hopping grid"},
    {"name": "airgrid.py", "needs": ("rx",), "span_hz": "grid",
     "what": "the six standing checks that the air matches the grid"},
)

# Permitted by the hardware, not written. Listed so the capability is visible.
# radio_devices_docs/open_hub/testing/sdr.md
TX_GATED = (
    ("pairing / join from a synthetic device",
     "stimulate the hub's receive path without a second board"),
    ("ACK and retry behaviour under controlled loss",
     "transmit uplinks and drop chosen ones"),
    ("hub receiver sensitivity",
     "step transmit power down until the hub stops hearing"),
    ("interferer and blocking response",
     "transmit an adjacent-channel interferer while the link runs"),
)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true",
                    help="one line naming the backend that would be chosen")
    a = ap.parse_args()

    rows = sdrdev.survey()
    ready = [r for r in rows if r["ready"]]

    if a.quiet:
        if not ready:
            print("none")
            return 1
        pick = max(ready, key=lambda r: r["backend"].max_usable_rate)
        print(pick["backend"].key)
        return 0

    print("On the bus")
    print("-" * 72)
    any_hw = False
    for r in rows:
        b = r["backend"]
        if not r["devices"]:
            print(f"  {b.label:10s} -")
            continue
        any_hw = True
        for d in r["devices"]:
            sn = d["serial"] or "(no serial)"
            print(f"  {b.label:10s} {d['model']}  usb {d['vid']}:{d['pid']}  sn={sn}")
    if not any_hw:
        print("  nothing this bench knows about")

    print()
    print("Can it be driven")
    print("-" * 72)
    for r in rows:
        mark = "yes" if r["ready"] else "NO "
        print(f"  {mark}  {r['backend'].label:10s} {r['reason']}")

    if not ready:
        print()
        print("No receiver can be driven, so every check below is blocked.")
        return 1

    pick = max(ready, key=lambda r: r["backend"].max_usable_rate)
    b = pick["backend"]
    caps = sdrdev.capabilities(b)
    span = sdrdev.grid_span_hz()

    print()
    print(f"Would capture with: {b.label}"
          + ("" if len(ready) == 1 else "  (widest span of the ready receivers)"))
    print("-" * 72)
    print(f"  samples      {caps['bits']}-bit, {caps['format']} "
          f"({sdrdev.fmt(caps['format'])['note']})")
    print(f"  usable span  {caps['usable_span_hz']/1e6:.2f} MHz"
          f"   (preferred rate {b.preferred_rate/1e6:.2f} Msps)")
    print(f"  tuning       {b.freq_range[0]/1e6:.0f}-{b.freq_range[1]/1e6:.0f} MHz")
    print(f"  transmit     {'yes' if caps['tx'] else 'no'}")
    print(f"  lock         {b.lock_path()}")
    if caps["grid_total"]:
        verdict = "the whole grid fits" if caps["covers_grid"] else \
                  "SHORT of the grid - the rest is not silent, it is unseen"
        print(f"  grid         {caps['grid_channels']} of {caps['grid_total']} "
              f"channels in one capture; needs {span/1e6:.2f} MHz - {verdict}")
    print(f"  gain         {b.gain_note()}")

    print()
    print("Checks")
    print("-" * 72)
    for t in TOOLS:
        need_span = span if t["span_hz"] == "grid" else 0
        blocked = None
        if "rx" in t["needs"] and not caps["rx"]:
            blocked = "no receiver"
        elif "tx" in t["needs"] and not caps["tx"]:
            blocked = f"{b.label} cannot transmit"
        elif need_span and caps["usable_span_hz"] < need_span:
            blocked = (f"partial: {caps['grid_channels']} of "
                       f"{caps['grid_total']} channels")
        if blocked is None:
            print(f"  run      {t['name']:14s} {t['what']}")
        else:
            print(f"  LIMITED  {t['name']:14s} {t['what']}")
            print(f"           {' ':14s} ^ {blocked}")

    print()
    if caps["tx"]:
        print(f"Unlocked by {b.label} but NOT WRITTEN YET - transmit-side checks:")
        for name, why in TX_GATED:
            print(f"  todo     {name}")
            print(f"           ^ {why}")
        print("  These need a transmitter and the bench has never had one, so")
        print("  none of them exist. The capability is real; the tools are not.")
    else:
        print(f"{b.label} is receive-only, so the hub's own receive path - "
              f"pairing,\nACKs, retries - cannot be exercised from here at all.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
