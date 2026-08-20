# Joining

**Status: implemented and verified on air.**

## The bootstrap problem

A device that is not paired yet has no key. Without a key it cannot compute the
[hop sequence](hopping.md), so it cannot know where to listen. Something has to be
findable without a secret.

The options were: scan the whole band and hope to catch a beacon; have the hub
transmit an unencrypted beacon on every channel; or reserve one channel.

**A fixed join channel** was chosen. Scanning is slow and unreliable on a
battery node, and beaconing everywhere costs duty cycle on every channel forever.

## The channel

Grid slot **14 → 866.5 MHz**, reserved out of the 29-slot plan. The hopping set is
therefore the other **28** channels, and the two can never collide — that is a
property of `hop_slot_to_grid`, not a statistical hope.

The middle of the band was chosen deliberately: it is the position furthest from
both sub-band edges, so a cheap device's crystal error has the most room before it
pushes the signal outside 865–868 MHz.

## When the hub transmits there

**Only while an operator has a pairing window open.** `device add <id> <fingerprint>` opens it for
60 seconds. During the window a join beacon goes out every **second** superframe.

| | Duty cycle contribution |
|---|---|
| Window closed | 0 |
| Window open | 0.21% |

A permanent join beacon would cost that 0.21% around the clock for nothing. Half
rate rather than every superframe keeps the cost down while still letting a device
find the hub inside two superframes.

**A device that was merely power-cycled does not need this channel at all.** It
still has its key, and any data beacon carries the superframe counter it needs to
re-align. The join channel is only for a device that has no key yet — which is
always a deliberate action by a human at both ends.

## The join beacon

```c
typedef struct radio_join_beacon {
    uint8_t  type;          /* RADIO_FRAME_JOIN_BEACON */
    uint8_t  version;
    uint16_t net_id;        /* public: it only has to identify the network */
    uint32_t hub_id;
    uint32_t superframe;
    uint8_t  flags;         /* RADIO_JOIN_FLAG_WINDOW_OPEN */
    uint8_t  hop_channels;  /* size of the hop set, so the plan needs no guessing */
} __attribute__((packed)) radio_join_beacon_t;
```

14 bytes. It is **cleartext by necessity** — it is the one frame a device must be
able to read before it has a key — so it carries nothing secret: which network,
which hub, what time it is, and how big the hop set is.

Carrying `superframe` means the joiner is already time-aligned by the moment it
has a key, and can follow the hop sequence immediately instead of resynchronising
afterwards.

## Pairing is non-blocking

The previous implementation, `RFM_add_device_routine`, sat in a loop for up to ten
seconds waiting for a response. That is incompatible with a slot grid — it stalls
every other radio activity for 10 000 slot-times.

`RFM_open_pairing` only sets a deadline, and the join traffic happens in the
**join region** at the tail of each superframe — 1 874 000 µs after the boundary,
past the last uplink slot. The hub retunes there, beacons, listens for 100 ms and
retunes back with 126 ms to spare. See [pairing.md](pairing.md).

The 100 ms receive window is split across superloop passes rather than blocked
on: sitting in one iteration for 100 ms would straddle a superframe boundary and
put that jitter into the beacon every device measures its period from.

The window only opens the *listening*. The exchange itself suspends the grid for
four superframes — a [quiesce](pairing.md), announced in the data beacon and
triggered by the device rather than by the operator.

`RFM_open_pairing` only sets a deadline:

```c
if (pairing_open && timebase_elapsed(pairing_deadline_us)) {
    pairing_open = 0;
    if (pair_state == RADIO_PAIR_LISTEN)
        pair_state = RADIO_PAIR_IDLE;
}
```

and `join_region_service()` does the rest at the region offset. The beacon goes
out through the ordinary transmit path and the window closes on its own deadline.

## Verification

Decoded off the air, byte for byte, on a 250 kS/s capture of the join channel:

```
0e 02 02 01 00 11 22 44 33 6e 01 00 00 01 1c
 |  |  |  |     |           |           |  +-- hop_channels 28
 |  |  |  |     |           |           +----- flags 0x01 WINDOW_OPEN
 |  |  |  |     |           +----------------- superframe 366
 |  |  |  |     +----------------------------- hub_id 0x33442211
 |  |  |  +----------------------------------- net_id 0x0001
 |  |  +-------------------------------------- version 2
 |  +----------------------------------------- type 0x02 JOIN_BEACON
 +-------------------------------------------- length 14
```

Three consecutive frames carried superframes **366, 368, 370** — incrementing by
two, which is the half-rate this page claims, measured rather than asserted.

Position within the superframe, measured on a wideband capture as the offset from
each data beacon to the join beacon that follows it:

```
nominal grid offset 1874.0 ms
measured 1873.9 .. 1874.8 ms over 5 frames (mean 1874.4)
```

Earlier, with the join beacon sent immediately after the data beacon and filtering
bursts to 866.5 MHz:

| | Packets on 866.5 MHz |
|---|---|
| Window closed | **0** |
| Window open | **8**, spaced 4008 ms, 8.27 ms each |

4008 ms is exactly two 2004 ms superframes. 8.27 ms matches the air time of a
14-byte frame at 25 kbps with preamble and sync. A ninth burst appeared once at a
different length and spacing — an unrelated interferer, not the hub.

**Four false negatives so far, none of them in the firmware.** Every one presented
as "the feature does not work", which is why they are all listed:

- a locale where `awk` truncated `866.4977` to `866` because it expected a comma;
- a decimation that filtered the signal *after* downsampling, destroying the
  kernel;
- a rotation that moved the wanted channel to +100 kHz and then low-passed at
  96 kHz, deleting it — caught only by pushing a frame known to decode through
  the same path as a control;
- `capture.py`'s defaults putting an FSK tone exactly on the Nyquist edge, so one
  tone of two survived and the discriminator produced nothing. The default
  offset is now 60 kHz, which leaves room at the only narrowband rate the
  hardware supports;
- `-s 500e3`, a rate the RTL2832U cannot produce — `rtl_sdr` warns and captures
  at whatever rate was last programmed, and the `.meta` claimed otherwise, so
  every duration downstream was silently scaled. Refused now;
- `find_bursts` thresholding at a quarter of the **peak**, which lands below the
  noise when the signal is only ~12 dB above it. Every burst then merges into
  one. The threshold is now the higher of a peak fraction and a floor multiple.

The firmware's own transmission counters are what kept the last two from turning
into a firmware hunt: `device pair` said 48 join beacons sent and 0 errors while the
capture said zero received, and only one of those two can be a bench problem.

## Open issue: the join beacon is unauthenticated

By definition. It is the only frame readable without a key, so anyone can transmit
one and impersonate a hub.

This is not fixable at the beacon layer, and it is not meant to be. Authentication
comes from what happens next: the key exchange binds to a device public key the
hub obtained out of band, so a forged beacon leads a device nowhere. But a forged
beacon **can** be used to keep a joining device busy, and that is accepted rather
than solved.

See [security/threat-model.md](../security/threat-model.md) and
[security/key-lifecycle.md](../security/key-lifecycle.md).

## See also

- [ADR-0009](../decisions/0009-fixed-join-channel.md)
- [ADR-0020](../decisions/0020-device-triggered-quiesce.md)
- [hopping.md](hopping.md)
- [pairing.md](pairing.md)
