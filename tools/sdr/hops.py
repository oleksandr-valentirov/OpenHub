#!/usr/bin/env python3
"""Track a frequency-hopping transmitter across a wideband capture.

Detection runs on a spectrogram rather than an amplitude envelope: a burst that
occupies 75 kHz inside a 2.4 MHz capture is buried in the wideband noise in the
time domain, but stands well clear of the floor in its own bin.

Reports where each burst actually sat, which channel of the configured grid that
is, and the duty cycle.
"""
import argparse

import numpy as np

import iqfile
import phy


def spectrogram(x, rate, nfft):
    n = len(x) // nfft
    win = np.hanning(nfft).astype(np.float32)
    P = np.empty((n, nfft), dtype=np.float32)
    for i in range(n):
        P[i] = np.abs(np.fft.fftshift(np.fft.fft(x[i * nfft:(i + 1) * nfft] * win))) ** 2
    return P, np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate))


def air_time_envelope(x, rate, s_slot, e_slot, slot_s, f_rel, bw=60e3, guard=12):
    """Re-measure one burst's air time on its own narrowband envelope.

    The spectrogram quantises to whole slots, so it can only report air time in
    multiples of nfft/rate - on a 2.4 Msps capture that is 0.853 ms against an
    8.5 ms frame, and coarser on a slower capture. It is the right tool for
    finding and attributing bursts across the band and a poor one for timing a
    single burst, so timing is taken from the envelope instead.
    """
    n = len(x)
    fallback = (e_slot - s_slot) * slot_s
    pad = int(guard * slot_s * rate)
    a = max(0, int(s_slot * slot_s * rate) - pad)
    b = min(n, int(e_slot * slot_s * rate) + pad)
    seg = x[a:b]
    if len(seg) < 4 * pad or pad < 16:
        return fallback

    # bring this burst's channel to DC, then keep only its own bandwidth
    t = np.arange(len(seg), dtype=np.float32) / rate
    seg = seg * np.exp(-2j * np.pi * f_rel * t).astype(np.complex64)
    env = np.abs(iqfile.lowpass(seg, rate, bw)).astype(np.float32)

    # The floor comes from the padding, never from the segment.
    # radio_devices_docs/open_hub/testing/sdr.md
    edge = np.concatenate((env[:pad // 2], env[-pad // 2:]))
    floor = float(np.median(edge))
    peak = float(np.percentile(env, 98))
    if peak <= floor * 1.5:
        return fallback

    on = np.flatnonzero(env > (floor + 0.5 * (peak - floor)))
    if len(on) < 2:
        return fallback
    return float(on[-1] - on[0]) / rate




def detect(path, nfft=2048, snr=15.0, bridge_ms=5.0, min_ms=2.0,
           base=None, spacing=None, count=None):
    """Every burst in the band, with where it sat and which channel that is.

    Split out so airgrid.py measures the same air this tool reports on. Two
    implementations of one detector are two answers, and the disagreement reads
    as progress.

    Each burst carries the set of channels lit during it, not only its strongest.
    Two radios overlapping in time make one interval with one peak, and the
    quieter one then reads as absent - which is a frequency mismatch that is not
    there. radio_devices_docs/open_hub/testing/sdr.md
    """
    c = phy.constants()
    base = c["RADIO_CH_BASE_HZ"] if base is None else base
    spacing = c["RADIO_CH_SPACING_HZ"] if spacing is None else spacing
    count = c["RADIO_GRID_COUNT"] if count is None else count

    meta = iqfile.read_meta(path)
    raw = np.fromfile(path, dtype=np.uint8).astype(np.float32) - 127.5
    x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)
    x -= x.mean()                      # kill the RTL-SDR centre spike
    rate, centre = meta["rate"], meta["centre"]

    P, freqs = spectrogram(x, rate, nfft)
    dB = 10 * np.log10(P + 1e-12)
    # Per-bin medians: the noise floor is not flat across 2.4 MHz.
    # radio_devices_docs/open_hub/testing/sdr.md
    floor = np.median(dB, axis=0)
    excess = dB - floor
    active = excess.max(axis=1) > snr
    slot_s = nfft / rate

    # Bins to channels, so a burst names every channel it lit.
    ch_of_bin = np.round((centre + freqs - base) / spacing).astype(int)

    raw_b, start = [], None
    for i, act in enumerate(active):
        if act and start is None:
            start = i
        elif not act and start is not None:
            raw_b.append((start, i))
            start = None
    if start is not None:
        raw_b.append((start, len(active)))

    # An FSK burst arrives as fragments; bridge, then drop what is too short.
    # radio_devices_docs/open_hub/testing/sdr.md
    bridge = max(1, int(round(bridge_ms * 1e-3 / slot_s)))
    merged = []
    for b in raw_b:
        if merged and b[0] - merged[-1][1] <= bridge:
            merged[-1] = (merged[-1][0], b[1])
        else:
            merged.append(b)
    bursts = [b for b in merged if (b[1] - b[0]) * slot_s >= min_ms * 1e-3]

    recs = []
    for s, e in bursts:
        band = P[s:e].sum(axis=0)
        pk = int(np.argmax(band))
        # centre of mass over the occupied bins, so the two FSK tones average out
        lo, hi = max(0, pk - 80), min(len(band), pk + 80)
        w = band[lo:hi]
        strong = w > w.max() * 0.15
        f_rel = float(np.sum(freqs[lo:hi][strong] * w[strong]) / np.sum(w[strong]))
        abs_hz = centre + f_rel
        idx = int(round((abs_hz - base) / spacing))
        seg = excess[s:e].max(axis=0)
        lit = set()
        for k in range(count):
            sel = ch_of_bin == k
            if sel.any() and float(seg[sel].max()) > snr:
                lit.add(k)
        recs.append({"lit": lit, "s": s, "e": e, "t_ms": s * slot_s * 1e3,
                     "air_ms": (e - s) * slot_s * 1e3, "hz": abs_hz, "ch": idx,
                     "err_hz": abs_hz - (base + idx * spacing),
                     "f_rel": f_rel, "on_grid": 0 <= idx < count})
    return {"x": x, "rate": rate, "centre": centre, "slot_s": slot_s,
            "floor": floor, "bursts": recs, "P": P, "freqs": freqs}

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--base", type=float, default=None, help="channel 0 centre, Hz")
    ap.add_argument("--spacing", type=float, default=None)
    ap.add_argument("--count", type=int, default=None)
    ap.add_argument("--nfft", type=int, default=2048)
    ap.add_argument("--snr", type=float, default=15.0, help="threshold above the per-bin floor, dB")
    ap.add_argument("--bridge-ms", type=float, default=5.0, help="merge fragments closer than this")
    ap.add_argument("--min-ms", type=float, default=2.0, help="discard bursts shorter than this")
    ap.add_argument("--expect-ms", type=float, default=None,
                    help="air time of one of our frames; bursts within --tol-ms of it "
                         "are counted as ours and reported separately")
    ap.add_argument("--tol-ms", type=float, default=1.5,
                    help="how far an air time may sit from --expect-ms")
    ap.add_argument("--channel", type=int, action="append", default=None,
                    metavar="N",
                    help="count only bursts on grid channel N; repeatable. For a "
                         "node parked on one channel this identifies it far better "
                         "than air time does. Combines with --expect-ms.")
    a = ap.parse_args()

    d = detect(a.path, a.nfft, a.snr, a.bridge_ms, a.min_ms,
               a.base, a.spacing, a.count)
    x, rate, centre = d["x"], d["rate"], d["centre"]
    slot_s, recs = d["slot_s"], d["bursts"]
    c = phy.constants()
    base = c["RADIO_CH_BASE_HZ"] if a.base is None else a.base
    print(f"capture {len(x)/rate:.2f} s at {rate/1e6:.3f} Msps, centred {centre/1e6:.3f} MHz")
    print(f"spectrogram slot {a.nfft/rate*1e3:.3f} ms - air times below are quantised to it")
    if not recs:
        raise SystemExit(f"nothing {a.snr:.0f} dB above the per-bin floor")
    bursts = [(r["s"], r["e"]) for r in recs]
    print(f"floor {d['floor'].mean():.1f} dB (per-bin), {len(bursts)} burst(s)\n")
    print(f"{'t (ms)':>10} {'air (ms)':>9} {'MHz':>11} {'ch':>5} {'err kHz':>8}  gap")
    prev, seen, bad = None, [], 0
    chan_of = {}
    frel_of = {}
    for r in recs:
        s, e, idx = r["s"], r["e"], r["ch"]
        if not r["on_grid"]:
            bad += 1
        seen.append(idx)
        chan_of[(s, e)] = idx
        frel_of[(s, e)] = r["f_rel"]
        gap = "" if prev is None else f"{(s-prev)*slot_s*1e3:8.1f} ms"
        print(f"{r['t_ms']:10.2f} {r['air_ms']:9.2f} {r['hz']/1e6:11.4f} "
              f"{idx:4d}{'!' if not r['on_grid'] else ' '} {r['err_hz']/1e3:8.1f}  {gap}")
        prev = s

    total = sum((e - s) * slot_s for s, e in bursts)
    span = len(x) / rate
    print(f"\nall bursts:   {100*total/span:.3f} %   {len(set(seen))} distinct channel(s)")

    # The total counts every burst in the band, ours and everyone else's.
    # radio_devices_docs/open_hub/testing/sdr.md
    if a.expect_ms is not None or a.channel is not None:
        mine, why = bursts, []
        if a.expect_ms is not None:
            mine = [b for b in mine
                    if abs((b[1] - b[0]) * slot_s * 1e3 - a.expect_ms) <= a.tol_ms]
            why.append(f"{a.expect_ms:.1f} +/- {a.tol_ms:.1f} ms")
        if a.channel is not None:
            mine = [b for b in mine if chan_of.get(b) in a.channel]
            why.append("ch " + ",".join(str(c) for c in a.channel))

        quant = sum((e - s) * slot_s for s, e in mine)
        # Timed on the envelope, not on whole spectrogram slots.
        ours = sum(air_time_envelope(x, rate, s, e, slot_s, frel_of[(s, e)])
                   for s, e in mine)
        print(f"ours ({'; '.join(why)}): {100*ours/span:.3f} %   "
              f"{len(mine)} of {len(bursts)} burst(s)")
        if mine:
            print(f"  air {1e3*ours/len(mine):.2f} ms/burst on the envelope, "
                  f"{1e3*quant/len(mine):.2f} ms quantised "
                  f"({100*(quant-ours)/ours:+.0f} %)")

        # A selection is trustworthy only if its cadence looks like one radio.
        # radio_devices_docs/open_hub/testing/sdr.md
        if len(mine) > 2:
            gaps = sorted((mine[i + 1][0] - mine[i][0]) * slot_s * 1e3
                          for i in range(len(mine) - 1))
            modal = gaps[len(gaps) // 2]
            spread = gaps[-1] - gaps[0]
            if spread < 0.05 * modal:
                print(f"  cadence {gaps[0]:.1f}-{gaps[-1]:.1f} ms (consistent)")
            else:
                # A multiple of the modal gap is a burst thrown away, not let in.
                # radio_devices_docs/open_hub/testing/sdr.md
                holes = [g for g in gaps
                         if abs(g / modal - round(g / modal)) < 0.02 and g > 1.5 * modal]
                if len(holes) == len(gaps) - gaps.count(modal) or holes:
                    print(f"  cadence {gaps[0]:.1f}-{gaps[-1]:.1f} ms (GAPS) - "
                          f"{len(holes)} gap(s) are whole multiples of {modal:.1f} ms: "
                          f"the filter dropped our own burst, loosen it")
                else:
                    print(f"  cadence {gaps[0]:.1f}-{gaps[-1]:.1f} ms (SCATTERED) - "
                          f"irregular, so the filter admitted another transmitter, "
                          f"tighten it")

        other = len(bursts) - len(mine)
        if other:
            print(f"{other} burst(s) excluded")
    if bad:
        print(f"WARNING: {bad} burst(s) outside the {a.count}-channel grid")


if __name__ == "__main__":
    main()
