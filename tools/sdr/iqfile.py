"""Shared loading and burst detection for the IQ captures.

Two receivers write two sample formats - the RTL-SDR unsigned 8-bit, the
bladeRF signed 12-in-16 - and which one a file holds is read from its `.meta`,
never guessed from the file. A capture written before the format line existed
is unsigned 8-bit, because that is the only thing that wrote one; that is what
`sdrdev.DEFAULT_FORMAT` says and it is the reason the older captures in this
directory still load.

Samples come back in the source's own ADC counts rather than normalised. That
is deliberate: `hops.py` decides whether a burst clipped by asking whether
samples sit on the outermost code, and the outermost code is a property of the
part. Normalising here would move that judgement into a scale factor and hide
it. Ask `rail_threshold()` for the number instead of writing one down.
"""
import os

import numpy as np

import sdrdev

# Everything else in a .meta is an integer.
_STR_KEYS = ("format", "backend", "serial")


def read_meta(path):
    meta = {"centre": None, "rate": 250000, "signal": None, "offset": 100000,
            "format": sdrdev.DEFAULT_FORMAT, "backend": None, "serial": None,
            "gain": None, "bandwidth": None}
    mpath = path + ".meta"
    if os.path.exists(mpath):
        for line in open(mpath):
            k, _, v = line.strip().partition("=")
            if k in _STR_KEYS:
                meta[k] = v
            elif k in meta:
                try:
                    meta[k] = int(v)
                except ValueError:
                    meta[k] = float(v)
    return meta


def rail_threshold(meta):
    """|sample| at or above this sat on an ADC rail, in this file's own counts.

    127.0 on the RTL-SDR, 2047.0 on the bladeRF. Reading a level off a clipped
    burst measures the receiver rather than the transmitter, and a 30 dB
    transmitter change once read as +0.07 dB for exactly this reason - so the
    threshold has to follow the file, not the tool that last worked.
    """
    return sdrdev.fmt(meta["format"])["rail"]


def fullscale(meta):
    """Magnitude of the outermost code, once centred."""
    return sdrdev.fmt(meta["format"])["fullscale"]


def load_raw(path, meta=None):
    """Interleaved real samples in the file's own counts, centred, plus its meta.

    Interleaved rather than complex because the rail test counts I and Q
    separately - either one hitting a rail clips the sample.
    """
    meta = read_meta(path) if meta is None else meta
    f = sdrdev.fmt(meta["format"])
    raw = np.fromfile(path, dtype=np.dtype(f["dtype"])).astype(np.float32)
    if f["centre"]:
        raw -= f["centre"]
    return raw, meta


def load(path, shift_to_dc=True):
    """Return (complex64 samples at baseband, sample rate)."""
    raw, meta = load_raw(path)
    x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)
    if shift_to_dc and meta["offset"]:
        n = np.arange(len(x), dtype=np.float64)
        x *= np.exp(-2j * np.pi * meta["offset"] * n / meta["rate"]).astype(np.complex64)
    return x, meta["rate"]


def write_meta(path, centre, rate, signal, offset, sample_format,
               backend=None, serial=None, gain=None, bandwidth=None):
    """Write the sidecar that tells every other tool how to read the capture.

    The backend, serial and gain are provenance, not decoration: a level quoted
    without naming the instrument it came off is half a fact, and two receivers
    on one bench is exactly when that stops being a slogan.
    """
    with open(path + ".meta", "w") as fh:
        fh.write(f"centre={int(centre)}\nrate={int(rate)}\n"
                 f"signal={int(signal)}\noffset={int(offset)}\n"
                 f"format={sample_format}\n")
        if backend:
            fh.write(f"backend={backend}\n")
        if serial:
            fh.write(f"serial={serial}\n")
        if gain is not None:
            fh.write(f"gain={gain}\n")
        if bandwidth is not None:
            fh.write(f"bandwidth={int(bandwidth)}\n")
    return path + ".meta"


def lowpass(x, rate, cutoff, taps=129):
    h = np.sinc(np.arange(-(taps // 2), taps // 2 + 1) * 2 * cutoff / rate)
    h *= np.hamming(taps)
    h /= h.sum()
    return np.convolve(x, h, "same")


def find_bursts(x, rate, rel_threshold=0.25, min_len_us=200, floor_mult=3.0,
                bridge_us=0):
    """Split the capture into transmit bursts by amplitude.

    Returns a list of (start, end) sample indices.

    The threshold is taken from the noise floor as well as from the peak, and
    the higher of the two wins. A fraction of the peak alone is right only while
    the signal dominates the capture: when the strongest thing present is close
    to the floor - a distant transmitter, or one seen through a filter wider
    than its channel - a quarter of the peak lands *below* the noise, half the
    samples read as "on", and every burst merges into one. That failure looks
    identical to a transmitter that never keyed, and it cost this project two
    sessions of hunting a firmware bug that did not exist.

    The floor is a low percentile rather than the median so that a capture which
    is mostly signal still measures its noise rather than its traffic.

    Fragments closer together than `bridge_us` are merged; it is off by default.
    Near the floor the envelope of a single frame dips below any threshold
    several times, and an 8 ms frame arriving as six sub-millisecond pieces is
    the same false negative by another route. But bridging noise crossings joins
    the whole capture into one burst, so how much to bridge is a judgement about
    the signal in hand and belongs to the caller - which is how `hops.py` has
    always treated it.

    A generous bridge is a decoding aid, not a measurement: `find_sync` will
    still locate a frame inside an over-merged blob and the bytes are good, but
    the burst's start and length are then meaningless. Do not read air time off
    a bridged burst.
    """
    env = np.abs(x)
    peak = env.max()
    if peak <= 0:
        return []
    floor = float(np.percentile(env, 20))
    on = env > max(peak * rel_threshold, floor * floor_mult)
    edges = np.diff(on.astype(np.int8))
    starts = list(np.where(edges == 1)[0])
    ends = list(np.where(edges == -1)[0])
    if ends and (not starts or ends[0] < starts[0]):
        ends.pop(0)
    n = min(len(starts), len(ends))
    spans = [[int(s), int(e)] for s, e in zip(starts[:n], ends[:n])]

    bridge = int(rate * bridge_us / 1e6)
    merged = []
    for span in spans:
        if merged and span[0] - merged[-1][1] <= bridge:
            merged[-1][1] = span[1]
        else:
            merged.append(span)

    # Length is judged after merging, never before.
    # radio_devices_docs/open_hub/testing/sdr.md
    min_len = int(rate * min_len_us / 1e6)
    return [(s, e) for s, e in merged if e - s >= min_len]


def spectrogram(x, rate, nfft=2048):
    n = len(x) // nfft
    win = np.hanning(nfft).astype(np.float32)
    P = np.empty((n, nfft), dtype=np.float32)
    for i in range(n):
        P[i] = np.abs(np.fft.fftshift(np.fft.fft(x[i * nfft:(i + 1) * nfft] * win))) ** 2
    return P, np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate))


def find_bursts_wideband(x, rate, nfft=2048, snr_db=15.0, bridge_ms=5.0, min_ms=2.0):
    """Find bursts anywhere in the captured band.

    find_bursts() lowpasses around the capture centre, so it only ever sees one
    channel. That is useless for a hopping transmitter: the hub's bursts land on
    28 channels across 2.8 MHz and almost none sit at the centre. Measured on a
    real capture, the lowpass path reported 0.011 % where the true figure was
    0.418 % - and printed a pass either way.

    Detection is on a spectrogram instead, each bin judged against its own
    median because the noise floor is not flat across 2.4 MHz.

    Returns (bursts, P, freqs, slot_s); bursts are (start_slice, end_slice).
    """
    P, freqs = spectrogram(x, rate, nfft)
    dB = 10 * np.log10(P + 1e-12)
    floor = np.median(dB, axis=0)
    active = (dB - floor).max(axis=1) > snr_db
    slot_s = nfft / rate

    bursts, start = [], None
    for i, act in enumerate(active):
        if act and start is None:
            start = i
        elif not act and start is not None:
            bursts.append((start, i))
            start = None
    if start is not None:
        bursts.append((start, len(active)))

    # An FSK burst dips below threshold whenever its energy is in the other tone.
    bridge = max(1, int(round(bridge_ms * 1e-3 / slot_s)))
    merged = []
    for b in bursts:
        if merged and b[0] - merged[-1][1] <= bridge:
            merged[-1] = (merged[-1][0], b[1])
        else:
            merged.append(b)

    bursts = [b for b in merged if (b[1] - b[0]) * slot_s >= min_ms * 1e-3]
    return bursts, P, freqs, slot_s
