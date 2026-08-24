#!/usr/bin/env python3
"""Per-channel occupancy of a wideband capture, on the grid and beside it.

`airgrid.py` asks whether the transmissions match the slot grid and reports what
did not fit as one undifferentiated list. That list is where an interferer hides:
a channel somebody else is sitting on looks exactly like a burst on no grid
position, and the two have opposite consequences - one is a defect in this
system, the other is a reason a run is void.

This reads occupancy per channel instead, so a foreign carrier is named by the
channel it occupies rather than counted among this system's own misses. It also
looks **outside** the grid, which only became possible when the bladeRF replaced
the RTL-SDR: 4 MHz spans indices -6..34 against a grid of 0..28, and an
adjacent-band emitter that a 2.4 MHz window could not see is one of the
candidates for a receiver being desensitised.

It is an analysis tool. It reads a capture that is already on disk and changes
nothing about what was recorded, which is what makes it writable during a run.

Empty population: a capture in which no channel clears the floor is a **failure**,
never a clean band - it is the signature of a receiver that was not tuned, not
enabled, or capturing into a terminator.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np

import iqfile
import phy


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?")
    ap.add_argument("--nfft", type=int, default=1024)
    ap.add_argument("--floor-db", type=float, default=10.0,
                    help="how far above the capture's own median a bin counts as occupied")
    ap.add_argument("--busy", type=float, default=1.0,
                    help="percent occupancy at which a channel is called busy")
    ap.add_argument("--self-test", action="store_true",
                    help="exercise the empty-band and single-carrier arms and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.path:
        ap.error("a capture path is required unless --self-test is given")

    x, rate = iqfile.load(args.path)
    meta = iqfile.read_meta(args.path)
    c = phy.constants()
    base, spacing = c["RADIO_CH_BASE_HZ"], c["RADIO_CH_SPACING_HZ"]
    grid = c["RADIO_GRID_COUNT"]
    signal = meta["signal"] or (base + (grid // 2) * spacing)

    dc_off = -(meta["offset"] or 0)
    flat = meta["bandwidth"] or rate
    rows, floor, frame_ms, span, blind = occupancy(
        x, rate, signal, base, spacing, args.nfft, args.floor_db, dc_off,
        meta["centre"], flat)

    print("%s" % args.path)
    print("  %s at %s, gain %s, %.1f Msps, %.1f s"
          % (meta["backend"], _mhz(meta["centre"]), meta["gain"],
             rate / 1e6, len(x) / rate))
    print("  noise floor (median bin) %.1f dB, occupied = floor + %.0f dB"
          % (floor, args.floor_db))
    print("  channel indices %d..%d are inside the captured band; the grid is 0..%d"
          % (span[0], span[1], grid - 1))
    trusted = [r["idx"] for r in rows if r["trusted"]]
    print("  analog filter %.2f MHz, so only channels %d..%d are evidence"
          % (flat / 1e6, min(trusted), max(trusted)))
    if flat >= rate:
        print("  WARNING: the analog filter is not narrower than the sample rate.")
        print("  Its transition band is inside the analysed span and reads as")
        print("  occupancy. Re-capture with --bandwidth below the rate.")
    ongrid_untrusted = [r["idx"] for r in rows if r["ongrid"] and not r["trusted"]]
    if ongrid_untrusted:
        print("  REFUSED for %d grid channel(s) outside the flat region: %s"
              % (len(ongrid_untrusted), ongrid_untrusted))
    print()

    occupied = [r for r in rows if r["pct"] >= args.busy and r["trusted"]]
    if not any(r["pct"] > 0 for r in rows):
        print("REFUSED: not one channel cleared the floor.")
        print("  An empty band is not a clean band - it is a receiver that was")
        print("  not tuned, not enabled, or capturing into a terminator.")
        return 2

    print("  ch      MHz    on-air %%   peak dB   median dB   where")
    for r in rows:
        if r["pct"] < args.busy and not r["ongrid"]:
            continue
        if r["pct"] < 0.05:
            continue
        if not r["trusted"]:
            where = "BEYOND FILTER - not evidence"
        else:
            where = "grid" if r["ongrid"] else "OUTSIDE"
        if r["trusted"] and r["ongrid"] and r["idx"] == c["RADIO_JOIN_SLOT"]:
            where = "grid, join"
        if r["blind"]:
            where += ", DC-NOTCHED"
        print("  %3d  %8.3f   %7.2f   %7.1f   %9.1f   %s"
              % (r["idx"], r["hz"] / 1e6, r["pct"], r["peak"], r["med"], where))

    print()
    off = [r for r in occupied if not r["ongrid"]]
    on = [r for r in occupied if r["ongrid"]]
    n_graded = len([r for r in rows if r["ongrid"] and r["trusted"]])
    print("  %d of %d graded grid channels busy at >= %.1f%%"
          % (len(on), n_graded, args.busy))
    if blind is not None:
        print("  channel %d carries the receiver's own DC spike and is notched out."
              % blind)
        print("  ^ this tool is BLIND on it. Retune with a different --offset to see it.")
    if off:
        print("  %d channel(s) busy OUTSIDE the grid: %s"
              % (len(off), ", ".join("%d (%.3f MHz, %.2f%%)"
                                     % (r["idx"], r["hz"] / 1e6, r["pct"]) for r in off)))
        print("  ^ these are somebody else's. A run whose window overlaps them is void")
        print("    for any absolute level, and suspect for any loss figure.")
    return 0


def occupancy(x, rate, signal_hz, base, spacing, nfft, floor_db, dc_off_hz=0.0,
              centre_hz=None, flat_hz=None):
    """Per-channel on-air percentage, by pile-up over an STFT.

    Bins are assigned to the nearest channel centre and a channel's occupancy is
    the fraction of frames in which **any** of its bins clears the floor. Nearest
    centre rather than a filter bank because the question here is *which channel
    is somebody on*, not what the burst contained - and a filter bank would put
    the answer behind a passband nobody chose.

    **The receiver's own DC bin is notched out before anything is counted.** Both
    receivers on this bench are zero-IF and both put a spike at the centre they
    were tuned to; `capture.py` already tunes off-channel so the spike misses the
    signal, but it still lands on *some* channel, and that channel then reads
    100% occupied at the loudest peak in the capture. The first run of this tool
    reported exactly that and it was the instrument, not the air. The notched
    channel is named in the output rather than silently dropped, because a
    channel this tool cannot see is not a channel that is quiet.

    **Only the analog filter's flat region is evidence.** A capture whose sample
    rate equals its analog bandwidth puts the filter's transition band inside the
    span being analysed, and the rolloff there reads as occupancy: 12-20% on four
    grid channels, which is indistinguishable from a foreign emitter sitting on
    them. That is not a hypothetical - it is what this tool reported on its first
    run, and re-centring the receiver moved the "traffic" to the new band edges
    while the grid channels fell to 0.2%. Channels outside the flat region are
    reported as BEYOND FILTER and are never counted as somebody else's traffic.
    """
    step = nfft // 2
    n = (len(x) - nfft) // step
    if n < 2:
        raise SystemExit("capture too short for %d-point frames at this rate" % nfft)

    win = np.hanning(nfft).astype(np.float32)
    acc_hot = None
    acc_pk = np.full(nfft, -np.inf, dtype=np.float32)
    acc_sum = np.zeros(nfft, dtype=np.float64)
    dbs = []

    chunk = max(1, 2_000_000 // nfft)
    for i in range(0, n, chunk):
        m = min(chunk, n - i)
        blk = np.empty((m, nfft), dtype=np.complex64)
        for j in range(m):
            s = (i + j) * step
            blk[j] = x[s:s + nfft]
        blk = blk * win
        p = np.abs(np.fft.fftshift(np.fft.fft(blk, axis=1), axes=1)) ** 2
        d = (10.0 * np.log10(p + 1e-12)).astype(np.float32)
        dbs.append(d.astype(np.float16))
        acc_pk = np.maximum(acc_pk, d.max(axis=0))
        acc_sum += d.sum(axis=0)

    db = np.concatenate([d.astype(np.float32) for d in dbs], axis=0)
    floor = float(np.median(db))
    hot = db > (floor + floor_db)

    binhz = rate / nfft
    freqs = (np.arange(nfft) - nfft // 2) * binhz + signal_hz
    idx = np.rint((freqs - base) / spacing).astype(int)
    lo, hi = int(idx.min()), int(idx.max())

    # The zero-IF spike, two bins either side of where the receiver was tuned.
    blind = None
    if dc_off_hz is not None:
        dc_bin = int(np.argmin(np.abs(freqs - (signal_hz + dc_off_hz))))
        w = 2
        keep = np.ones(nfft, dtype=bool)
        keep[max(0, dc_bin - w):dc_bin + w + 1] = False
        blind = int(idx[dc_bin])
        hot = hot & keep[None, :]

    rows = []
    grid = phy.constants()["RADIO_GRID_COUNT"]
    # 90% of the half-bandwidth: the corner is where a filter starts, not ends.
    half_flat = 0.45 * flat_hz if flat_hz else None
    for k in range(lo, hi + 1):
        sel = idx == k
        if not sel.any():
            continue
        frames_hot = hot[:, sel].any(axis=1)
        rows.append({
            "idx": k,
            "hz": base + k * spacing,
            "pct": 100.0 * float(frames_hot.mean()),
            "peak": float(db[:, sel].max()),
            "med": float(np.median(db[:, sel])),
            "ongrid": 0 <= k < grid,
            "blind": k == blind,
            "trusted": (half_flat is None or centre_hz is None
                        or abs((base + k * spacing) - centre_hz) <= half_flat),
        })
    return rows, floor, step / rate * 1000.0, (lo, hi), blind


def _mhz(hz):
    return "%.3f MHz" % (hz / 1e6) if hz else "?"


def self_test():
    """Both arms, because a check that has never refused reads in neither direction."""
    rate, nfft = 4_000_000, 1024
    c = phy.constants()
    base, spacing = c["RADIO_CH_BASE_HZ"], c["RADIO_CH_SPACING_HZ"]
    signal = base + 14 * spacing
    n = rate  # one second

    rng = np.random.default_rng(7)
    noise = (rng.normal(size=n) + 1j * rng.normal(size=n)).astype(np.complex64) * 0.01

    rows, _, _, _, _ = occupancy(noise, rate, signal, base, spacing, nfft, 10.0,
                                 0.0, signal, rate)
    quiet = max(r["pct"] for r in rows)
    ok_empty = quiet < 5.0

    t = np.arange(n) / rate
    tone_idx = 22
    off = (base + tone_idx * spacing) - signal
    carrier = np.exp(2j * np.pi * off * t).astype(np.complex64)
    rows2, _, _, _, _ = occupancy(noise + carrier, rate, signal, base, spacing,
                                  nfft, 10.0, 0.0, signal, rate)
    hit = [r for r in rows2 if r["idx"] == tone_idx][0]
    neigh = max(r["pct"] for r in rows2 if abs(r["idx"] - tone_idx) > 1)
    ok_tone = hit["pct"] > 95.0 and neigh < 20.0

    print("noise only, no carrier:      busiest channel %6.2f%%   want < 5      %s"
          % (quiet, "ok" if ok_empty else "FAIL"))
    print("one carrier on channel %d:   that channel   %6.2f%%   want > 95     %s"
          % (tone_idx, hit["pct"], "ok" if ok_tone else "FAIL"))
    print("                             busiest other  %6.2f%%   want < 20"
          % neigh)
    print()
    if ok_empty and ok_tone:
        print("bandscan answers both ways")
        return 0
    print("bandscan FAILED its own self-test")
    return 1


if __name__ == "__main__":
    sys.exit(main())
