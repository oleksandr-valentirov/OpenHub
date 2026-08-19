"""2-FSK demodulation and bit recovery for the RFM69 link.

The RFM69 sends preamble and sync word raw; only the payload carries the
DcFree coding, so sync is searched on the raw bitstream and decoding of the
payload happens afterwards.
"""
import numpy as np

from iqfile import lowpass


def discriminate(seg, rate, bandwidth):
    """FSK discriminator output, one sample per input sample, DC removed."""
    y = lowpass(seg, rate, bandwidth)
    inst = np.angle(y[1:] * np.conj(y[:-1]))
    return inst - np.median(inst)  # median kills residual carrier offset


def slice_bits(inst, rate, bitrate):
    """Sample the discriminator at the phase that opens the eye widest."""
    sps = rate / bitrate
    best = None
    for phase in np.arange(0, sps, 0.25):
        idx = np.arange(phase, len(inst) - 1, sps).astype(int)
        score = np.mean(np.abs(inst[idx]))
        if best is None or score > best[0]:
            best = (score, idx)
    return (inst[best[1]] > 0).astype(np.uint8)


def bits_to_string(bits):
    return "".join("1" if b else "0" for b in bits)


def find_sync(bitstr, sync_bytes):
    """Return the bit offset just past the sync word, or -1."""
    pattern = "".join(f"{b:08b}" for b in sync_bytes)
    pos = bitstr.find(pattern)
    return -1 if pos < 0 else pos + len(pattern)


def pack_bytes(bitstr):
    n = len(bitstr) // 8 * 8
    return bytes(int(bitstr[i:i + 8], 2) for i in range(0, n, 8))


def manchester_decode(bitstr, phase=0):
    """RFM69 DcFree=Manchester: the first half-bit carries the data."""
    return "".join(bitstr[i] for i in range(phase, len(bitstr) - 1, 2))


def dewhiten(data):
    """Undo the RFM69 DcFree=Whitening 9-bit LFSR (x^9 + x^5 + 1, seed 0x1FF)."""
    lfsr = 0x1FF
    out = bytearray()
    for byte in data:
        mask = 0
        for _ in range(8):
            mask = (mask << 1) | (lfsr & 1)
            feedback = ((lfsr >> 0) ^ (lfsr >> 5)) & 1
            lfsr = (lfsr >> 1) | (feedback << 8)
        out.append(byte ^ mask)
    return bytes(out)


def crc16_ccitt(data, init=0x1D0F):
    """RFM69 packet CRC: CCITT poly 0x1021, seed 0x1D0F."""
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
