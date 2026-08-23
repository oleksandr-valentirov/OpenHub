# SDR test bench

Validation of the hub's radio from the air rather than from a counter: carrier
accuracy, FSK deviation, bit rate, framing, burst timing and regulatory duty
cycle.

**Two receivers reach this bench and they do not answer the same questions.**
Ask which one is here before planning a measurement:

```bash
.venv/bin/python sdrinfo.py     # what is plugged in, and what it can run
```

| | RTL-SDR | bladeRF |
|---|---|---|
| samples | 8-bit, `u8`, rails at 127 | 12-bit, `sc16q11`, rails at 2047 |
| usable span | 2.4 MHz — **25 of the 29 channels** | 40 MHz — the whole grid at once |
| transmit | no | **yes** |
| host software | `apt install rtl-sdr` | `apt install bladerf libbladerf2 bladerf-fpga-hostedx40 bladerf-firmware-fx3` |

The RTL-SDR cannot transmit, so on it the hub's **receive** path — pairing,
ACKs, retries — is out of scope. A bladeRF removes that limit; the tools for it
are **not written yet**, and `sdrinfo.py` lists them as `todo` rather than
pretending otherwise.

`capture.py` picks the receiver itself and records which one it used, at what
gain, in what format, in the `.meta` beside the capture. Nothing downstream
guesses: the format is read from the file's `.meta`, and a capture written
before that line existed is `u8`, because that is the only thing that wrote one.

> **The rail is the receiver's, not the tool's.** `hops.py` calls a sample
> clipped when it sits on the outermost ADC code — 127 on the RTL-SDR, 2047 on
> the bladeRF. A literal in the tool reads a bladeRF capture as either wholly
> clipped or never clipped, and a rail fraction is exactly the number that
> decides whether a dB column may be quoted at all. Ask
> `iqfile.rail_threshold(meta)`.

> **The `-w` bandwidth default tracks the PHY.** It is 60 kHz for the current
> 25 kbps / 25 kHz-deviation profile. Set too narrow, the tools do not fail —
> they under-report: bursts fragment and get discarded, and deviation reads
> wrong. Move it whenever the PHY moves.

## Sharing the receivers

Each receiver claims its USB interface exclusively, so a second capture kills
the first attempt. Hub work and sensor device work happen in separate sessions,
so `capture.py` takes an `flock` for the duration of a capture and names the
holder to whoever is blocked:

```
RTL-SDR busy: hub session, pid 62981, since 11:05:14
Wait, or raise --wait. Lock: /tmp/openhub-rtlsdr.lock
```

**The lock is per receiver, not per bench** — two radios can be captured at
once and serialising them would be an invented constraint:

| Receiver | Lock |
|---|---|
| RTL-SDR | `/tmp/openhub-rtlsdr.lock` |
| bladeRF | `/tmp/openhub-bladerf.lock` |

The RTL-SDR's path is **unchanged** from when it was the only receiver, because
other sessions and anything calling `rtl_sdr` directly take that exact path.
Renaming it would leave the guard looking present and doing nothing.

`--label` says who you are, `--wait N` blocks for up to N seconds instead of
giving up. Override the path with `OPENHUB_SDR_LOCK`. **Anything calling
`rtl_sdr` or `bladeRF-cli` directly must take the matching lock**, or the guard
is worthless.

**Detection never opens the device.** `sdrinfo.py` reads USB ids out of sysfs;
`rtl_test` and `bladeRF-cli -p` would each claim the interface, and asking
"what is plugged in?" must not be able to end another session's 120 s capture.

The lock stops two captures colliding. It does **not** stop the other side's
transmitter from appearing in your capture — see the duty-cycle caveat below,
where a bench beacon in another repository inflated a measurement here.

## Setup

```bash
python3 -m venv .venv && .venv/bin/pip install numpy
```

`rtl_sdr` (package `rtl-sdr`) or `bladeRF-cli` (package `bladerf`) must be on
`PATH` for the matching receiver. `sdrinfo.py` names the missing package rather
than letting the capture fail obscurely.

**A bladeRF 1 loads its FPGA image at every power-up.** Without
`bladerf-fpga-hostedx40` (or `-hostedx115`) it enumerates, opens, and will not
stream — which reads exactly like a dead antenna. `capture.py` recognises that
failure and says so.

## Use

```bash
.venv/bin/python sdrinfo.py                          # which receiver, which checks
.venv/bin/python capture.py cap.iq -f 868e6 -t 5     # writes cap.iq + cap.iq.meta
.venv/bin/python spectrum.py cap.iq                  # carrier offset, FSK deviation
.venv/bin/python decode.py cap.iq                    # frames, byte by byte
.venv/bin/python dutycycle.py cap.iq                 # ETSI sub-band check, exits 1 on fail
```

`capture.py` tunes below the signal on purpose: both receivers are zero-IF and
both put a DC spike at their centre frequency, which would otherwise sit on top
of the carrier. The offset is recorded in the `.meta` file and undone by the
other tools. `-d rtlsdr` / `-d bladerf` forces a receiver instead of letting it
choose; a receiver asked for and absent is an error, never a silent fallback to
the other one, whose format and span are not what the caller asked for.

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
| `sdrdev.py` | Which receivers exist, what each can do, how to drive it |
| `sdrinfo.py` | What is plugged in **now** and which checks it can run |
| `capture.py` | Drives whichever receiver is present, records tuning metadata |
| `iqfile.py` | Loads captures, shifts to baseband, splits into bursts |
| `gfsk.py` | FSK discriminator, bit slicing, sync search, Manchester/whitening, CRC |
| `decode.py` | Frame dump per burst |
| `dutycycle.py` | Duty cycle against EN 300 220 sub-band limits |
| `spectrum.py` | Averaged burst spectrum as an ASCII plot |
| `hops.py` | Follows a hopping transmitter across a wideband capture |
