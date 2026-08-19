# SDR test bench

Receive-side validation of the hub's radio using an RTL-SDR. Everything the hub
*transmits* can be checked here without a second board: carrier accuracy, FSK
deviation, bit rate, framing, burst timing and regulatory duty cycle.

The RTL-SDR cannot transmit, so the hub's **receive** path — pairing, ACKs,
retries — needs a real second radio and is out of scope for these tools.

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

Defaults track the current PHY (9.6 kbps, sync `hell`, Manchester). Override as
the profile changes:

```bash
.venv/bin/python decode.py cap.iq -b 25000 -w 60e3 --coding whitening --sync 0x2dd4
```

## Files

| File | Role |
|---|---|
| `capture.py` | Drives `rtl_sdr`, records tuning metadata |
| `iqfile.py` | Loads captures, shifts to baseband, splits into bursts |
| `gfsk.py` | FSK discriminator, bit slicing, sync search, Manchester/whitening, CRC |
| `decode.py` | Frame dump per burst |
| `dutycycle.py` | Duty cycle against EN 300 220 sub-band limits |
| `spectrum.py` | Averaged burst spectrum as an ASCII plot |
