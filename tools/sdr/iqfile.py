"""Shared loading and burst detection for the raw u8 IQ captures."""
import os

import numpy as np


def read_meta(path):
    meta = {"centre": None, "rate": 250000, "signal": None, "offset": 100000}
    mpath = path + ".meta"
    if os.path.exists(mpath):
        for line in open(mpath):
            k, _, v = line.strip().partition("=")
            if k in meta:
                meta[k] = int(v)
    return meta


def load(path, shift_to_dc=True):
    """Return (complex64 samples at baseband, sample rate)."""
    meta = read_meta(path)
    raw = np.fromfile(path, dtype=np.uint8).astype(np.float32) - 127.5
    x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)
    if shift_to_dc and meta["offset"]:
        n = np.arange(len(x), dtype=np.float64)
        x *= np.exp(-2j * np.pi * meta["offset"] * n / meta["rate"]).astype(np.complex64)
    return x, meta["rate"]


def lowpass(x, rate, cutoff, taps=129):
    h = np.sinc(np.arange(-(taps // 2), taps // 2 + 1) * 2 * cutoff / rate)
    h *= np.hamming(taps)
    h /= h.sum()
    return np.convolve(x, h, "same")


def find_bursts(x, rate, rel_threshold=0.25, min_len_us=200):
    """Split the capture into transmit bursts by amplitude.

    Returns a list of (start, end) sample indices.
    """
    env = np.abs(x)
    peak = env.max()
    if peak <= 0:
        return []
    on = env > peak * rel_threshold
    edges = np.diff(on.astype(np.int8))
    starts = list(np.where(edges == 1)[0])
    ends = list(np.where(edges == -1)[0])
    if ends and (not starts or ends[0] < starts[0]):
        ends.pop(0)
    n = min(len(starts), len(ends))
    min_len = int(rate * min_len_us / 1e6)
    return [(int(s), int(e)) for s, e in zip(starts[:n], ends[:n]) if e - s >= min_len]
