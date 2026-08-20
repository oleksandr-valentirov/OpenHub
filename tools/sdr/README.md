# SDR test bench

Receive-side validation of the hub's radio using an RTL-SDR. Everything the hub
*transmits* can be checked here without a second board: carrier accuracy, FSK
deviation, bit rate, framing, burst timing and regulatory duty cycle.

The RTL-SDR cannot transmit, so the hub's **receive** path — pairing, ACKs,
retries — needs a real second radio and is out of scope for these tools.

> **The `-w` bandwidth default tracks the PHY.** It is 60 kHz for the current
> 25 kbps / 25 kHz-deviation profile. Set too narrow, the tools do not fail —
> they under-report: bursts fragment and get discarded, and deviation reads
> wrong. Move it whenever the PHY moves.

## Sharing the dongle

There is **one** RTL-SDR on this machine and `rtl_sdr` claims the USB interface
exclusively, so a second capture kills the first attempt. Hub work and sensor
device work happen in separate sessions, so `capture.py` takes an `flock` on
`/tmp/openhub-rtlsdr.lock` for the duration of a capture and names the holder to
whoever is blocked:

```
RTL-SDR busy: hub session, pid 62981, since 11:05:14
Wait, or raise --wait. Lock: /tmp/openhub-rtlsdr.lock
```

`--label` says who you are, `--wait N` blocks for up to N seconds instead of
giving up. Override the path with `OPENHUB_SDR_LOCK`. **Anything calling
`rtl_sdr` directly must take the same lock**, or the guard is worthless.

The lock stops two captures colliding. It does **not** stop the other side's
transmitter from appearing in your capture — see the duty-cycle caveat below,
where a bench beacon in another repository inflated a measurement here.

## Setup

```bash
python3 -m venv .venv && .venv/bin/pip install numpy
```

`rtl_sdr` from the `rtl-sdr` package must be on `PATH`.

## Use

```bash
.venv/bin/python capture.py cap.iq -f 868e6 -t 5     # writes cap.iq + cap.iq.meta
.venv/bin/python spectrum.py cap.iq                  # carrier offset, FSK deviation
.venv/bin/python decode.py cap.iq                    # frames, byte by byte
.venv/bin/python dutycycle.py cap.iq                 # ETSI sub-band check, exits 1 on fail
```

`capture.py` tunes 100 kHz below the signal on purpose: the RTL-SDR has a large
DC spike at its centre frequency that would otherwise sit on top of the carrier.
The offset is recorded in the `.meta` file and undone by the other tools.

Defaults in `decode.py` still describe the old 9.6 kbps Manchester link; the hub
now runs 25 kbps with whitening, so pass those:

```bash
.venv/bin/python decode.py cap.iq -b 25000 -w 60e3 --coding whitening
```

## Frequency hopping

A hopping transmitter needs a capture wide enough to hold the whole grid, and
detection then has to run on a spectrogram: a 75 kHz burst inside a 2.4 MHz
capture disappears into the wideband noise if you look at the amplitude
envelope instead.

```bash
.venv/bin/python capture.py hop.iq -f 866.3e6 -s 2400000 -t 12 --offset 0
.venv/bin/python hops.py hop.iq --base 865.1e6 --spacing 100e3 --count 29
```

Each burst is reported with the channel index it lands on and the error against
the grid; a few tens of kHz is normal, since the measurement sees the FSK tones
rather than the suppressed carrier. Expect some bursts to be missed at the edges
of a wide capture, which makes the duty-cycle figure here a lower bound - use
`dutycycle.py` scans the whole captured band by default, which is what a hopping
transmitter needs. `--narrow HZ` restores the old lowpass-at-the-centre behaviour,
which is correct only when the transmitter stays on the capture centre frequency —
a fixed-channel test, or the join channel. Measured on a hopping capture the
narrow path reported 0.011 % against a true 0.418 %, and printed a pass.

## Files

| File | Role |
|---|---|
| `capture.py` | Drives `rtl_sdr`, records tuning metadata |
| `iqfile.py` | Loads captures, shifts to baseband, splits into bursts |
| `gfsk.py` | FSK discriminator, bit slicing, sync search, Manchester/whitening, CRC |
| `decode.py` | Frame dump per burst |
| `dutycycle.py` | Duty cycle against EN 300 220 sub-band limits |
| `spectrum.py` | Averaged burst spectrum as an ASCII plot |
| `hops.py` | Follows a hopping transmitter across a wideband capture |
