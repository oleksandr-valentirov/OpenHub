#!/usr/bin/env python3
"""Cross-check that the air matches the slot grid the headers describe.

The firmware's counters sit above the packet engine and cannot see a frame the
part refused, so "the device stopped transmitting" and "the hub stopped hearing"
look identical from here. This reads the transmissions off the air instead and
holds them against the geometry in Common/inc, which is the one artifact both
firmwares compile.

Every burst is classified by its **phase in the superframe** and never by its
duration. Duration is then checked separately, as an assertion rather than as
the label: a control classified by the quantity it measures can only agree, and
this bench has already reported the hub's own downlink as the device's uplink
because the two are the same length.

The phase itself is solved by pile-up, which is a fit and therefore needs its own
control - a wrong grid still finds some best phase. The peak is reported against
the background it stands out from, and a flat score across phases fails C1
rather than being read as a weak result.

C5 asks whether the hub's channel was lit during the burst rather than whether
it won the peak, because a louder neighbour overlapping in time would otherwise
be reported as the device having hopped wrong.

radio_devices_docs/open_hub/testing/sdr.md
"""
import argparse

import numpy as np
import sys

import hops
import phy


def positions(c, device):
    """Every position a superframe should carry, as (name, microseconds)."""
    p = [("beacon", c["RADIO_BEACON_OFFSET_US"], phy.constants()["sizeof radio_data_beacon_t"]),
         ("downlink", c["RADIO_DOWNLINK_OFFSET_US"], c["RADIO_DOWNLINK_BYTES"])]
    for k, s in enumerate(phy.uplink_slots(device)):
        p.append(("uplink k=%d slot %d" % (k, s), phy.slot_offset_us(s),
                  c["RADIO_UPLINK_BYTES"]))
    return p


def solve_grid(bursts, nominal_us, pos_us, tol_us, ppm=600.0, n_ph=400, n_pd=241):
    """Phase and period together, because the two clocks are not the same clock.

    The capture's time axis comes from the dongle's crystal and the superframe
    from the hub's, so a fixed 2 s period walks off by milliseconds over a minute
    - 213 ppm measured on the first capture, which is ordinary for an untrimmed
    RTL-SDR. Solving for the period as well turns that drift from a limit on the
    check into a reading: the hub's superframe, timed by an outside clock.

    Returns (phase, period, hits, background), where background is the median
    score over all candidates - a grid that does not describe the air scores
    flat, and C1 reads the ratio rather than the peak alone.
    """
    t_us = np.array([b[0] * 1000.0 for b in bursts], dtype=np.float64)
    pos = np.array(pos_us, dtype=np.float64)
    best = (0.0, nominal_us, -1)
    scores = []
    for j in range(n_pd):
        period = nominal_us * (1.0 + (2.0 * j / (n_pd - 1) - 1.0) * ppm * 1e-6)
        ph = np.linspace(0.0, period, n_ph, endpoint=False)
        rel = (t_us[None, :] - ph[:, None]) % period          # (n_ph, n_bursts)
        d = np.abs(rel[:, :, None] - pos[None, None, :]).min(axis=2)
        hits = (d < tol_us).sum(axis=1)
        scores.append(hits)
        k = int(hits.argmax())
        if hits[k] > best[2]:
            best = (float(ph[k]), float(period), int(hits[k]))
    return best[0], best[1], best[2], float(np.median(np.concatenate(scores)))


def anchor_superframes(bursts, ph, period, boot_tol):
    """Give every superframe its own origin, taken from its own beacon.

    A single (phase, period) does not hold across a capture: the beacon residual
    walked 10.7 ms over 30 superframes on the first one measured, and the scatter
    around any straight line was milliseconds, so bursts fall out of a tight
    window in the later part and land on the wrong position in the earlier part.

    The beacon sits at RADIO_BEACON_OFFSET_US of its own superframe by
    definition, so it is the origin rather than something to be located against
    one. The global fit is bootstrap only; origins come from the bursts.
    """
    n = int((bursts[-1][0] * 1000.0 - ph) // period) + 1
    origin = {}
    for i in range(n + 1):
        want = ph + i * period
        near = [b for b in bursts if abs(b[0] * 1000.0 - want) < boot_tol]
        if near:
            origin[i] = min(near, key=lambda b: abs(b[0] * 1000.0 - want))[0] * 1000.0
    # A beaconless superframe is interpolated, never dropped.
    known = sorted(origin)
    for i in range(n + 1):
        if i in origin or not known:
            continue
        lo = max([k for k in known if k < i], default=None)
        hi = min([k for k in known if k > i], default=None)
        if lo is not None and hi is not None:
            origin[i] = origin[lo] + (origin[hi] - origin[lo]) * (i - lo) / (hi - lo)
        elif lo is not None:
            origin[i] = origin[lo] + (i - lo) * period
        elif hi is not None:
            origin[i] = origin[hi] - (hi - i) * period
    return origin


def cycle_violations(seq, count):
    """A channel repeating inside one cycle, which hop_channel cannot produce.

    hop_channel returns deck[superframe % count] over a Fisher-Yates permutation,
    so within one cycle every channel appears at most once. The capture's indices
    are relative, so the cycle boundary is unknown - but there are only `count`
    places it can be, and the sequence is consistent with a permutation if **any**
    alignment puts no repeat inside a cycle.

    An earlier version asked whether one boundary explained every close repeat,
    which is only meaningful for a window shorter than two cycles; its self-test
    failed a clean permutation on the first run. `--self-test` exercises both
    answers, and records that a uniform index slip passes: this catches an
    inconsistent bookkeeping, never a constant offset.

    This is the only check here that tests the superframe bookkeeping rather than
    the channel estimate, and the bookkeeping is what any cross-instrument
    comparison rests on.
    """
    best = None
    for off in range(count):
        cyc = {}
        rep = []
        for i in sorted(seq):
            k = (i + off) // count
            seen = cyc.setdefault(k, {})
            if seq[i] in seen:
                rep.append((seen[seq[i]], i, seq[i]))
            else:
                seen[seq[i]] = i
        if not rep:
            return [], True
        if best is None or len(rep) < len(best):
            best = rep
    return best or [], False


def c0_self_test():
    """C0 has only ever refused. A check that has never passed reads in neither
    direction, so both its answers are exercised here on synthetic sequences."""
    import random
    n = phy.constants()["RADIO_GRID_COUNT"] - 1
    rnd = random.Random(20260822)
    seq, decks = {}, {}
    for sf in range(4 * n):
        cyc = sf // n
        if cyc not in decks:
            d = list(range(n)); rnd.shuffle(d); decks[cyc] = d
        seq[sf] = decks[cyc][sf % n]
    rep, ok = cycle_violations(seq, n)
    print("clean permutation over %d superframes: %s (%d repeat(s) inside a cycle)"
          % (len(seq), "passes" if ok else "FAILS", len(rep)))
    good = ok and not rep

    # One position forced to a value already used in its own cycle.
    bad = dict(seq)
    bad[5] = bad[1]
    rep2, ok2 = cycle_violations(bad, n)
    print("one duplicated position:            %s (%d repeat(s))"
          % ("passes" if ok2 else "refuses", len(rep2)))

    # A uniform slip is a relabelling, so C0 passes it. Stated, not hidden.
    slip = {sf: seq[sf + 1] if (sf + 1) in seq else seq[sf]
            for sf in range(2 * n)}
    rep3, ok3 = cycle_violations(slip, n)
    print("index slipped by one:               %s (%d repeat(s))"
          % ("passes" if ok3 else "refuses", len(rep3)))

    if good and not ok2:
        print("\nC0 answers both ways")
        return 0
    print("\nC0 IS VACUOUS: it cannot distinguish its two outcomes")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="a wideband .iq capture spanning several superframes")
    ap.add_argument("--device", type=int, default=1, help="the device slot to expect")
    # Half the slot pitch; wider and adjacent slots accept the same burst.
    ap.add_argument("--tol-us", type=float, default=None,
                    help="default: half RADIO_SLOT_US, and it is refused above that")
    ap.add_argument("--nfft", type=int, default=2048)
    ap.add_argument("--snr", type=float, default=15.0)
    # Sound captures gave 83-85 %, a 4 % slot-pitch error gave 58 %.
    ap.add_argument("--min-explained", type=float, default=0.70,
                    help="C1 fails below this fraction of bursts on a grid position")
    ap.add_argument("--self-test", action="store_true",
                    help="exercise C0 both ways on synthetic decks and exit")
    ap.add_argument("--mutate-slot-us", type=float, default=None,
                    help="SELF-TEST ONLY: corrupt the slot pitch, so that a passing "
                         "run proves the checks cannot fail and must be investigated")
    a = ap.parse_args()

    if a.self_test:
        return c0_self_test()

    c = dict(phy.constants())
    tol_max = c["RADIO_SLOT_US"] / 2.0
    if a.tol_us is None:
        a.tol_us = tol_max
    elif a.tol_us > tol_max:
        sys.exit("--tol-us %.0f exceeds half the slot pitch (%.0f); slot %d and "
                 "slot %d would both accept the same burst."
                 % (a.tol_us, tol_max, 1, 2))
    if a.mutate_slot_us is not None:
        sys.stderr.write("MUTATED: slot pitch %d -> %d us. A pass here is a defect.\n"
                         % (c["RADIO_SLOT_US"], a.mutate_slot_us))
        c["RADIO_SLOT_US"] = a.mutate_slot_us
        phy.slot_offset_us = lambda n, _c=c: _c["RADIO_UPLINK_OFFSET_US"] + n * _c["RADIO_SLOT_US"]

    d = hops.detect(a.path, nfft=a.nfft, snr=a.snr)
    rate, centre = d["rate"], d["centre"]
    bursts = [(r["t_ms"], r["air_ms"], r["hz"], r["ch"], r["lit"]) for r in d["bursts"]]

    lo, hi = centre - rate / 2.0, centre + rate / 2.0
    grid_lo = max(0, int(round((lo - c["RADIO_CH_BASE_HZ"]) / c["RADIO_CH_SPACING_HZ"])) + 1)
    grid_hi = min(c["RADIO_GRID_COUNT"] - 1,
                  int(round((hi - c["RADIO_CH_BASE_HZ"]) / c["RADIO_CH_SPACING_HZ"])) - 1)

    if not bursts:
        print("no bursts detected - nothing to check, and that is not a pass")
        return 2

    pos = positions(c, a.device)
    pos_us = [p[1] for p in pos]
    period = c["SUPERFRAME_US"]
    ph, period, best_n, med_n = solve_grid(bursts, period, pos_us, a.tol_us)

    span = (bursts[-1][0] - bursts[0][0]) * 1000.0
    frames = max(1, int(span // period) + 1)
    print("capture spans ~%d superframes, %d bursts detected" % (frames, len(bursts)))
    print("superframe measured against the dongle's clock: %.0f us (%+.0f ppm vs %d)"
          % (period, 1e6 * (period - c["SUPERFRAME_US"]) / c["SUPERFRAME_US"],
             c["SUPERFRAME_US"]))
    print("channels %d..%d are inside the captured band (%d of %d on the grid)\n"
          % (grid_lo, grid_hi, grid_hi - grid_lo + 1, c["RADIO_GRID_COUNT"]))

    origin = anchor_superframes(bursts, ph, period, period / 2.0)
    byframe, buckets, unexplained = {}, {p[0]: [] for p in pos}, []
    for b in bursts:
        us = b[0] * 1000.0
        idx = min(origin, key=lambda i: abs(us - origin[i])) if origin else 0
        if us < origin.get(idx, us):
            idx = idx - 1 if (idx - 1) in origin else idx
        rel = us - origin.get(idx, ph)
        hit = min(pos, key=lambda p: abs(rel - p[1]))
        if abs(rel - hit[1]) < a.tol_us:
            buckets[hit[0]].append(b)
            byframe.setdefault(idx, {})[hit[0]] = b
        else:
            unexplained.append((rel, b))

    fails = []
    hubseq = {}
    explained = 1.0 - len(unexplained) / float(len(bursts))
    peak = best_n / max(1.0, med_n)
    print("C1 phase pile-up: %.0f%% of bursts on a grid position, peak %.1fx the "
          "median phase" % (100 * explained, peak))
    if explained < a.min_explained:
        fails.append("C1 explained %.0f%% < %.0f%%" % (100 * explained, 100 * a.min_explained))
    if peak < 1.5:
        fails.append("C1 no phase stands out - the grid does not describe this air")

    print("\nposition                    n   air ms (mean)   predicted   channels")
    for name, off, payload in pos:
        v = buckets[name]
        pred = phy.air_us(payload) / 1000.0
        if not v:
            print("  %-24s %3d        -           %6.2f      -" % (name, 0, pred))
            continue
        mean_air = sum(b[1] for b in v) / len(v)
        print("  %-24s %3d     %6.2f          %6.2f      %s"
              % (name, len(v), mean_air, pred,
                 ",".join(str(x) for x in sorted({b[3] for b in v}))))

    ups = [p[0] for p in pos if p[0].startswith("uplink")]
    counts = [len(buckets[u]) for u in ups]
    print("\nC2 every position occupied: %s"
          % ("yes" if all(len(buckets[p[0]]) for p in pos) else "NO"))
    if not all(len(buckets[p[0]]) for p in pos):
        fails.append("C2 a grid position carried no burst at all")

    if not any(counts):
        print("C3 uplink opportunities equal: no evidence - not one uplink was seen")
        fails.append("C3 no uplink burst was detected at all")
    else:
        print("C3 uplink opportunities equal: %s  (%s)"
              % ("yes" if len(set(counts)) == 1 else "NO", counts))
        if min(counts) < 0.8 * max(counts):
            fails.append("C3 uplink opportunities differ by more than 20%%: %s" % counts)

    same = diff = 0
    for idx, d in byframe.items():
        chs = [d[u][3] for u in ups if u in d]
        if len(chs) < 2:
            continue
        if len(set(chs)) == 1:
            same += 1
        else:
            diff += 1
    print("C4 one channel per superframe: %d yes, %d no%s"
          % (same, diff, "" if same or diff else "  <- no evidence"))
    if not (same or diff):
        fails.append("C4 no superframe carried two uplinks to compare")
    if diff:
        fails.append("C4 %d superframe(s) split their uplinks across channels" % diff)

    # The downlink anchors the hub's channel; the beacon is checked apart.
    m = n = 0
    off_ref = []
    for idx, d in byframe.items():
        ref = d.get("downlink") or d.get("beacon")
        if ref is None:
            continue
        for u in ups:
            if u in d:
                n += 1
                # Lit at all counts: a louder neighbour hides a real frame.
                if ref[3] in d[u][4]:
                    m += 1
                else:
                    off_ref.append((idx, u, ref[3], d[u][3], ref[1], d[u][1]))
    print("C5 uplink channel follows the hub's own frame: %d of %d%s"
          % (m, n, "" if n else "  <- no evidence"))
    for idx, u, rc, uc, rair, uair in off_ref:
        print("     sf %-4d %-20s hub ch %2d (%.2f ms), uplink ch %2d (%.2f ms)"
              % (idx, u, rc, rair, uc, uair))
    if not n:
        fails.append("C5 no superframe carried both a hub frame and an uplink")
    if n and m < n:
        fails.append("C5 %d uplink(s) sat on a channel the hub did not name" % (n - m))

    for i, dd in byframe.items():
        ref0 = dd.get("downlink") or dd.get("beacon")
        if ref0 is not None:
            hubseq[i] = ref0[3]
    rep, ok = cycle_violations(hubseq, c["RADIO_GRID_COUNT"] - 1)
    print("C0 hub channel sequence is a permutation per cycle: %s"
          % ("yes" if ok else "NO - the superframe index is wrong, not the channel"))
    for a, b2, v in rep[:6]:
        print("     ch %2d at superframe %d and %d, %d apart" % (v, a, b2, b2 - a))
    if not ok:
        fails.append("C0 no cycle boundary explains %d repeat(s); the bookkeeping "
                     "is unsound and nothing below can be read across superframes"
                     % len(rep))

    bd = [(i, d["beacon"][3], d["downlink"][3]) for i, d in byframe.items()
          if "beacon" in d and "downlink" in d]
    agree = sum(1 for _, b, dl in bd if b == dl)
    print("C5b beacon and downlink agree: %d of %d%s"
          % (agree, len(bd), "" if bd else "  <- no evidence"))
    for i, b, dl in bd:
        if b != dl:
            print("     sf %-4d beacon ch %2d, downlink ch %2d" % (i, b, dl))
    if not bd:
        fails.append("C5b no superframe carried both a beacon and a downlink")

    slot_ms = period / 1000.0 / 1000.0
    bad = []
    for name, off, payload in pos:
        v = buckets[name]
        if not v:
            continue
        pred = phy.air_us(payload) / 1000.0
        mean_air = sum(b[1] for b in v) / len(v)
        if abs(mean_air - pred) > 2.5:
            bad.append("%s %.2f vs %.2f ms" % (name, mean_air, pred))
    checked = sum(1 for name, _, _ in pos if buckets[name])
    print("C6 air time matches the payload: %s  (%d of %d positions had bursts)"
          % ("yes" if not bad else "NO", checked, len(pos)))
    if checked == 0:
        fails.append("C6 no position carried a burst to time")
    if bad:
        fails.append("C6 " + "; ".join(bad))

    if unexplained:
        print("\n%d burst(s) on no grid position - foreign traffic shares this band:"
              % len(unexplained))
        for rel, b in sorted(unexplained)[:6]:
            print("   phase %8.1f ms  ch %2d  air %5.2f ms" % (rel / 1000.0, b[3], b[1]))

    print()
    if fails:
        for f in fails:
            print("FAIL: %s" % f)
        return 1
    print("all checks passed over %d superframes" % frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
