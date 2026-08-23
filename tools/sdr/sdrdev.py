#!/usr/bin/env python3
"""Which radio is plugged in, what it can do, and how to drive it.

Two receivers now reach this bench and they are not interchangeable. The
RTL-SDR is an 8-bit receive-only dongle whose usable span stops short of the
hopping grid; the bladeRF is a 12-bit full-duplex transceiver that covers the
whole grid in one capture and can transmit into the hub. A tool that assumes
either one produces a confident wrong answer on the other, which is the failure
mode this bench has paid for six times already - see the list at the top of the
`sdr` skill.

So the backend is data, not an assumption. Each one below declares its sample
format, its full-scale code, the rates it can really produce, the span it can
cover and whether it can transmit; every tool asks rather than restates. The
same rule that put the PHY constants behind `phy.py` puts the receiver
constants here: there is one definition and no second copy to go stale.

**Detection never opens the device.** It reads USB vendor and product ids out
of sysfs. `rtl_test` and `bladeRF-cli -p` would each claim the USB interface,
and on a bench where another session may be halfway through a 120 s capture,
asking "what is plugged in?" must not be able to end somebody's measurement.
The cost is that presence here means "the hardware is on the bus", not "the
hardware works" - so `readiness()` reports the host software separately, and
says which package is missing rather than letting the capture fail obscurely.
"""
import os
import pathlib
import re
import shutil
import subprocess

# Sample formats. The rail differs per receiver and is not a tool constant.
# radio_devices_docs/open_hub/testing/sdr.md

FORMATS = {
    # RTL2832U.
    "u8": {
        "dtype": "uint8",
        "centre": 127.5,     # subtracted on load to centre the samples
        "fullscale": 127.5,  # magnitude of the outermost code once centred
        "rail": 127.0,       # |v| >= rail means the sample hit a rail
        "bytes_per_sample": 2,
        "code_min": 0,       # the raw codes a writer must clip to
        "code_max": 255,
        "write_target": 120.0,  # peak a rewritten capture is scaled to
        "note": "unsigned 8-bit, 0..255, DC at 127.5",
    },
    # bladeRF.
    "sc16q11": {
        "dtype": "int16",
        "centre": 0.0,
        "fullscale": 2047.5,
        "rail": 2047.0,
        "bytes_per_sample": 4,
        "code_min": -2048,
        "code_max": 2047,
        "write_target": 1927.0,  # the headroom the u8 path uses, 120 of 127.5
        "note": "signed 16-bit carrying 12 bits, SC16 Q11, -2048..2047",
    },
}

DEFAULT_FORMAT = "u8"  # what a .meta without a format= line was written by


def fmt(name):
    if name not in FORMATS:
        raise ValueError(f"unknown sample format {name!r}; "
                         f"known: {', '.join(sorted(FORMATS))}")
    return FORMATS[name]


_SYSFS = pathlib.Path("/sys/bus/usb/devices")


def _usb_present(ids):
    """(vid, pid) pairs found on the bus, read from sysfs without opening anything."""
    found = []
    if not _SYSFS.is_dir():
        return found
    for d in _SYSFS.iterdir():
        try:
            vid = (d / "idVendor").read_text().strip().lower()
            pid = (d / "idProduct").read_text().strip().lower()
        except (OSError, UnicodeDecodeError):
            continue
        if (vid, pid) in ids:
            serial = ""
            try:
                serial = (d / "serial").read_text().strip()
            except (OSError, UnicodeDecodeError):
                pass
            found.append({"vid": vid, "pid": pid, "serial": serial,
                          "model": ids[(vid, pid)], "sysfs": str(d)})
    return found


class Backend:
    """One kind of receiver, described rather than assumed."""

    key = None
    label = None
    usb_ids = {}
    sample_format = DEFAULT_FORMAT
    can_tx = False
    # Rates the part produces; outside these is refused, not warned about.
    rate_ranges = ()
    # Known good rather than merely legal.
    preferred_rate = None
    max_usable_rate = None
    freq_range = (0, 0)
    tools = ()
    packages = ""
    # The DC spike the offset-tuning trick dodges.
    dc_spike = True
    default_offset = 60_000

    def lock_path(self):
        return os.environ.get("OPENHUB_SDR_LOCK", self._lock_default)

    def devices(self):
        return _usb_present(self.usb_ids)

    def present(self):
        return bool(self.devices())

    def missing_tools(self):
        return [t for t in self.tools if shutil.which(t) is None]

    def effective_bandwidth(self, rate, bandwidth):
        """The analog filter width actually used, or None if the part has none."""
        return None

    def check_rate(self, rate):
        """Return the rate, or a string saying why it cannot be used."""
        rate = int(rate)
        if any(lo <= rate <= hi for lo, hi in self.rate_ranges):
            return rate, None
        ranges = " or ".join(f"{lo}-{hi}" for lo, hi in self.rate_ranges)
        return None, (f"{self.label} cannot produce {rate} Hz "
                      f"(it does {ranges} Hz)")

    def check_freq(self, hz):
        lo, hi = self.freq_range
        if lo <= hz <= hi:
            return None
        return (f"{hz/1e6:.3f} MHz is outside {self.label}'s tuning range "
                f"({lo/1e6:.0f}-{hi/1e6:.0f} MHz)")

    def readiness(self):
        """(ready, reason) - hardware on the bus and host software installed."""
        if not self.present():
            return False, "not plugged in"
        missing = self.missing_tools()
        if missing:
            return False, (f"plugged in, but {', '.join(missing)} not on PATH "
                           f"- install: {self.packages}")
        return True, "ready"


class RtlSdr(Backend):
    key = "rtlsdr"
    label = "RTL-SDR"
    # librtlsdr's own table, trimmed to the dongles that turn up in practice.
    usb_ids = {
        ("0bda", "2832"): "Realtek RTL2832U",
        ("0bda", "2838"): "Realtek RTL2838 (R820T/R820T2)",
        ("0ccd", "00a9"): "Terratec Cinergy T Stick Black",
        ("0ccd", "00b3"): "Terratec NOXON DAB/DAB+ rev1",
        ("0ccd", "00d3"): "Terratec Cinergy T Stick RC",
        ("0ccd", "00e0"): "Terratec NOXON DAB/DAB+ rev2",
        ("1554", "5020"): "PixelView PV-DT235U",
        ("15f4", "0131"): "Astrometa DVB-T",
        ("185b", "0620"): "Compro Videomate U620F",
        ("1b80", "d3a4"): "Twintech UT-40",
        ("1d19", "1101"): "Dexatek DK DVB-T",
        ("1f4d", "b803"): "GTek T803",
        ("1f4d", "c803"): "Lifeview LV5TDeluxe",
    }
    sample_format = "u8"
    can_tx = False
    # The only rates the RTL2832U produces; one in the gap captures anyway.
    rate_ranges = ((225_001, 300_000), (900_001, 3_200_000))
    # 2.4 loses 0 samples per million here, 2.88 loses 241, measured with rtl_test.
    preferred_rate = 2_400_000
    max_usable_rate = 2_400_000
    freq_range = (24e6, 1766e6)
    tools = ("rtl_sdr",)
    packages = "apt install rtl-sdr"
    dc_spike = True
    default_offset = 60_000
    # Unchanged on purpose; other callers take this exact path.
    # radio_devices_docs/open_hub/testing/sdr.md
    _lock_default = "/tmp/openhub-rtlsdr.lock"

    def capture_cmd(self, path, centre, rate, seconds, gain, serial=None,
                    bandwidth=None):
        n = int(rate * seconds)  # rtl_sdr counts complex samples, not bytes
        cmd = ["rtl_sdr", "-f", str(int(centre)), "-s", str(int(rate)),
               "-g", str(gain), "-n", str(n)]
        if serial:
            cmd += ["-d", str(serial)]
        return [cmd + [path]]

    def gain_note(self):
        # -g 0 asks for AGC; this has cost a re-capture.
        return ("-g 0 is AUTOMATIC gain, not zero gain. The lowest manual value "
                "this R820T offers is 0.9; rtl_test prints the list.")


class BladeRf(Backend):
    key = "bladerf"
    label = "bladeRF"
    usb_ids = {
        ("2cf0", "5246"): "bladeRF 1 (x40/x115)",
        ("2cf0", "5250"): "bladeRF 2.0 micro (xA4/xA5/xA9)",
        ("1d50", "6066"): "bladeRF (legacy id)",
    }
    sample_format = "sc16q11"
    can_tx = True
    # LMS6002D; the 2.0 micro starts at 520 kHz and is narrowed in for_model().
    rate_ranges = ((160_000, 40_000_000),)
    preferred_rate = 4_000_000
    max_usable_rate = 40_000_000
    freq_range = (300e6, 3800e6)
    tools = ("bladeRF-cli",)
    packages = ("apt install bladerf libbladerf2 bladerf-fpga-hostedx40 "
                "bladerf-firmware-fx3")
    # Zero-IF too, so the same spike is there and the offset trick stays.
    dc_spike = True
    default_offset = 60_000
    _lock_default = "/tmp/openhub-bladerf.lock"

    def for_model(self, model):
        """Narrow the declared limits once the exact part is known."""
        if model and "2.0 micro" in model:
            self.rate_ranges = ((520_834, 61_440_000),)
            self.freq_range = (47e6, 6000e6)
            self.max_usable_rate = 61_440_000
        return self

    def effective_bandwidth(self, rate, bandwidth):
        return int(bandwidth if bandwidth else rate)

    def capture_cmd(self, path, centre, rate, seconds, gain, serial=None,
                    bandwidth=None):
        n = int(rate * seconds)
        # Narrower than the captured span attenuates edge channels into silence.
        # radio_devices_docs/open_hub/testing/sdr.md
        bw = int(bandwidth if bandwidth else rate)
        dev = f"*:serial={serial}" if serial else None
        script = [
            f"set frequency rx {int(centre)}",
            f"set samplerate rx {int(rate)}",
            f"set bandwidth rx {bw}",
            # Off by name, so the gain in the .meta is the gain that was used.
            "set agc rx off",
            f"set gain rx {int(round(gain))}",
            f"rx config file={path} format=bin n={n}",
            "rx start",
            "rx wait",
        ]
        cmd = ["bladeRF-cli"]
        if dev:
            cmd += ["-d", dev]
        for line in script:
            cmd += ["-e", line]
        return [cmd]

    def gain_note(self):
        return ("bladeRF RX gain is a unified dB figure (roughly 5-60 dB on the "
                "bladeRF 1). AGC is switched off explicitly by capture.py, so "
                "the number asked for is the number used.")

    # Image directory the Ubuntu bladerf-fpga-hosted* packages write to.
    fpga_dir = "/usr/share/Nuand/bladeRF"

    def ensure_tx_fpga(self, serial=None):
        """Reload the FPGA from the host image if the board configured itself.

        The image this board autoloads from its own SPI flash receives but will
        not transmit: a plain tone times out in the USB transfer layer, while
        the identical version loaded from the host streams cleanly. The failure
        is silent - frames simply never leave - so every transmit path checks
        this first rather than reporting an empty air.
        radio_devices_docs/open_hub/testing/sdr.md
        """
        def cli(*script):
            cmd = ["bladeRF-cli"]
            if serial:
                cmd += ["-d", "*:serial=%s" % serial]
            for line in script:
                cmd += ["-e", line]
            return subprocess.run(cmd, capture_output=True, text=True).stdout

        out = cli("info", "version")
        if "configured by USB host" in out:
            return None
        m = re.search(r"FPGA size:\s*(\d+)\s*KLE", out)
        if not m:
            raise SystemExit("cannot read the bladeRF's FPGA size; is it plugged in?")
        image = "%s/hostedx%s.rbf" % (self.fpga_dir, m.group(1))
        if not os.path.exists(image):
            raise SystemExit("%s is missing: apt install bladerf-fpga-hostedx%s"
                             % (image, m.group(1)))
        cli("load fpga %s" % image)
        if "configured by USB host" not in cli("version"):
            raise SystemExit("the FPGA would not load from %s" % image)
        return image


BACKENDS = (RtlSdr(), BladeRf())


def by_key(key):
    for b in BACKENDS:
        if b.key == key:
            return b
    raise SystemExit(f"unknown SDR backend {key!r}; "
                     f"known: {', '.join(b.key for b in BACKENDS)}")


def survey():
    """Every backend, with what is on the bus and whether it can be driven."""
    out = []
    for b in BACKENDS:
        devs = b.devices()
        if devs and isinstance(b, BladeRf):
            b.for_model(devs[0]["model"])
        ready, reason = b.readiness()
        out.append({"backend": b, "devices": devs, "ready": ready,
                    "reason": reason})
    return out


def select(prefer=None):
    """Pick the backend to capture with.

    `prefer` names one explicitly and then a missing one is an error, because a
    tool that silently falls back to the other radio produces a capture in a
    format and a span the caller did not ask for.

    Left to itself the choice is the widest-span ready receiver, which is the
    one that can answer the most questions. The choice is always printed by the
    caller: an instrument that picked itself and did not say so is half a fact.
    """
    rows = survey()
    if prefer and prefer != "auto":
        want = by_key(prefer)
        for r in rows:
            if r["backend"].key == want.key:
                if not r["ready"]:
                    raise SystemExit(
                        f"{want.label} was asked for but is {r['reason']}.\n"
                        f"Run sdrinfo.py to see what is available.")
                return r["backend"], r["devices"]
    ready = [r for r in rows if r["ready"]]
    if not ready:
        lines = [f"  {r['backend'].label:10s} {r['reason']}" for r in rows]
        raise SystemExit("no usable SDR:\n" + "\n".join(lines) +
                         "\nRun sdrinfo.py for the full picture.")
    ready.sort(key=lambda r: r["backend"].max_usable_rate, reverse=True)
    return ready[0]["backend"], ready[0]["devices"]


# Capabilities, derived from the part and from phy.py rather than listed.
# radio_devices_docs/open_hub/testing/sdr.md

def grid_span_hz():
    """Hz needed to hold the whole hopping grid, edge to edge, or None."""
    try:
        import phy
        c = phy.constants()
        return ((c["RADIO_GRID_COUNT"] - 1) * c["RADIO_CH_SPACING_HZ"]
                + 2.0 * phy.demod_cutoff())
    except Exception:
        return None


def capabilities(backend):
    """What this receiver can and cannot be asked, as plain facts."""
    f = fmt(backend.sample_format)
    usable = backend.max_usable_rate or 0
    caps = {
        "rx": True,
        "tx": backend.can_tx,
        "format": backend.sample_format,
        "bits": 8 if backend.sample_format == "u8" else 12,
        "fullscale": f["fullscale"],
        "usable_span_hz": usable,
        "covers_grid": None,
        "grid_channels": None,
        "grid_total": None,
    }
    span = grid_span_hz()
    if span is not None:
        try:
            import phy
            c = phy.constants()
            caps["grid_total"] = c["RADIO_GRID_COUNT"]
            # Whole channels inside the usable span.
            fit = int(usable // c["RADIO_CH_SPACING_HZ"]) + 1
            caps["grid_channels"] = min(fit, c["RADIO_GRID_COUNT"])
            caps["covers_grid"] = usable >= span
        except Exception:
            pass
    return caps


def gain_note_for(meta):
    """The gain trap that applies to whichever receiver wrote this capture.

    `-g 0` meaning AGC has already cost one re-capture on the RTL-SDR, so the
    warning is worth printing - but printing the RTL-SDR's warning over a
    bladeRF capture is how a tool teaches the wrong lesson confidently.
    """
    key = meta.get("backend") if isinstance(meta, dict) else None
    if not key:
        # No backend line on older captures; the format still names the part.
        f = (meta.get("format") if isinstance(meta, dict) else None) or DEFAULT_FORMAT
        key = "rtlsdr" if f == "u8" else "bladerf"
    for b in BACKENDS:
        if b.key == key:
            return b.gain_note()
    return "unknown receiver - check what manual gain it actually accepts."
