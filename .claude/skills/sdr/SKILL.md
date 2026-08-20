---
name: sdr
description: Use the RTL-SDR to see what the hub actually radiates - capture, demodulate, decode frames, decrypt sealed bodies with operator-supplied keys, and check duty cycle. Use whenever a radio claim needs evidence from the air rather than from a counter, and alongside the rfm69 skill when the two disagree.
---

# The SDR bench

`tools/sdr/` validates everything the hub **transmits** without a second board.
The dongle is receive-only, so the hub's receive path needs a real radio and is
out of scope here - use the `rfm69` skill's counters for that side.

**These two skills answer different questions and the pairing is the point:**
`rfm69` tells you what the part *thinks* it did; this tells you what left the
antenna. Every long-running fault in this project lived in the gap between them.

## Before believing a negative, run a control

**Six false negatives on this bench so far, every one in the tools rather than
the firmware.** Push a frame you know decodes through the same path first.

The ones that have already happened, all now fixed:

- a rotate-then-lowpass that deleted the signal
- a default offset putting an FSK tone on the Nyquist edge
- `-s 500e3`, a rate the RTL2832U cannot produce, which `rtl_sdr` warns about
  and then ignores while the `.meta` claims otherwise
- a burst threshold taken from the peak alone, which lands under the noise
- **`--bitrate` defaulting to 9.6 kbps** after the PHY moved to 25 kbps
- **`--coding` defaulting to `manchester`** after the hub dropped it

The last two are the shape to watch: **a stale default in a diagnostic tool is
worse than no default, because it produces a confident negative.** Whenever the
PHY moves, move the defaults with it and grep the tools for the old value.

## Sharing the dongle

One RTL-SDR, and `rtl_sdr` claims the USB interface exclusively. `capture.py`
takes an `flock` on `/tmp/openhub-rtlsdr.lock` and names whoever holds it.
`--label` says who you are, `--wait N` blocks. **Anything calling `rtl_sdr`
directly must take the same lock** or the guard is worthless.

The lock stops two captures colliding. It does **not** stop the other side's
transmitter appearing in yours - a bench beacon from the device repository has
already inflated a duty-cycle measurement here.

## The air split, agreed with the device session

```
866.5 MHz          join channel, grid slot 14 - hub only, and only with a
                   pairing window open
865.1-867.9 MHz    the 28-channel hopping grid
869.5 MHz          bench traffic, different sync word
```

**Say so before measuring duty cycle and let the other side hold transmit.** A
bench beacon on the join channel has cost this project two debugging sessions.

## Running it

```bash
python3 -m venv .venv && .venv/bin/pip install numpy
cd tools/sdr
../../.venv/bin/python capture.py cap.iq -f 868e6 -t 5 --label hub
../../.venv/bin/python decode.py cap.iq
../../.venv/bin/python dutycycle.py cap.iq     # exits 1 over the ETSI limit
```

`pluck.py` cuts one channel out of a wideband capture, which is what a hopping
transmitter needs. `hops.py` tracks the transmitter across the grid and reports
which channel each burst sat on - detection runs on a spectrogram, because a
75 kHz burst inside a 2.4 MHz capture is buried in the time domain.

**The hub reaches this dongle only ~12 dB above the noise floor** in the usual
placement, so thresholds taken from the peak alone land under the noise.

## Reading sealed traffic

Frames are AES-128-GCM. `decode.py --keys keys.json` opens them **when the
operator supplies the keys** - the tools derive nothing.

```json
{
  "session": "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
  "hop":     "23b66242d97b0b059b356447ba49895f",
  "dev_id":  "0xa5a5a5a5"
}
```

Get them from the hub: `device hop <sf>` prints the network hop key's head and
names it; the session key is per device and lives in CM7's keystore.

**`dev_id` has to be given.** It is not on the wire - the hub assigned the slot,
so the hub owns the slot-to-device map, and a bench decoder is outside that map.
The nonce is `superframe(4) || dev_id(4) || direction(1) || slot(3)`, all
big-endian, so a wrong `dev_id` fails the tag exactly like a wrong key.

**A failed tag is printed, not raised.** On a shared bench most frames belong to
someone else and a wrong key is the normal case, not an error.

**Keys in a file, not on the command line**, so they stay out of shell history.
Treat any key that has been through this path as burned: re-pair afterwards
rather than shipping a device whose session key sat in a debug file.

## What the decode gives you that a counter cannot

The hub's counters sit *above* the packet engine and cannot see a frame the part
refuses. The SDR sits below everything:

- **frames the hub transmits but the device never reports** - the frame is on
  air and the fault is in the device's receive path
- **frames on the wrong channel** - `hops.py` names the channel each burst sat
  on, against the grid the hub says it should be on
- **radiated power** - the finding that the hub was transmitting into an
  unbonded PA had been sitting in a capture for weeks as "the hub's beacon is
  1.1 dB over the floor while the device's is 43.9", noted and passed over
- **air time and duty cycle**, which no firmware counter measures because there
  is no in-firmware governor

## Keep this current

When the PHY moves, the tool defaults move. When a new frame type lands, it goes
in `tools/sdr/frames.py`. When a false negative is found, it goes in the list at
the top - **that list is the reason the next negative gets a control.**
