#!/usr/bin/env python3
"""Re-enumerates the ST-Link probes over USB, which is what revives a wedged console.

radio_devices_docs/open_hub/testing/on-target.md
"""
import argparse
import fcntl
import glob
import os
import sys
import time

USBDEVFS_RESET = ord("U") << 8 | 20
ST_VENDOR = "0483"

# The bench's three probes, so a serial can be given as a name.
PROBES = {
    "hub":   "0049004A3234510637333934",
    "nodeA": "004800273333511531363730",
    "nodeB": "002B003E3234510A33353533",
}


def find(serial):
    """Returns the /dev/bus/usb node for one ST-Link, or None."""
    for d in glob.glob("/sys/bus/usb/devices/*/"):
        try:
            if open(d + "idVendor").read().strip() != ST_VENDOR:
                continue
            if open(d + "serial").read().strip() != serial:
                continue
            bus = int(open(d + "busnum").read())
            dev = int(open(d + "devnum").read())
            return "/dev/bus/usb/%03d/%03d" % (bus, dev)
        except OSError:
            continue
    return None


def reset(serial):
    """Issues USBDEVFS_RESET. The probe re-enumerates; the target is not touched."""
    node = find(serial)
    if node is None:
        return "not on the bus"
    fd = os.open(node, os.O_WRONLY)
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    finally:
        os.close(fd)
    return node


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("who", nargs="*", default=sorted(PROBES),
                    help="hub, nodeA, nodeB, a serial, or nothing for all three")
    args = ap.parse_args()
    bad = 0
    for name in args.who:
        serial = PROBES.get(name, name)
        out = reset(serial)
        print("  %-6s %s" % (name, out))
        if out == "not on the bus":
            bad += 1
    if bad == 0:
        time.sleep(4)
        print("\nconsoles now on the bus:")
        for link in sorted(glob.glob("/dev/serial/by-id/*STLINK*")):
            print("  %s -> %s" % (os.path.basename(link), os.path.realpath(link)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
