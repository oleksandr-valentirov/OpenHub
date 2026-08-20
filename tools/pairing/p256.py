"""Minimal P-256, written here rather than imported.

An independent implementation is the point of the exercise: a generator that
calls a shared library would agree with the other side's shared library and
learn nothing about the specification. It is checked against wire_v3's pinned
keys and ECDH output before it is used for anything new.
"""

P  = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A  = P - 3
B  = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5
N  = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
G  = (GX, GY)


def on_curve(pt):
    if pt is None:
        return True
    x, y = pt
    return (y * y - (x * x * x + A * x + B)) % P == 0


def add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, P - 2, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def mul(k, pt):
    r = None
    while k:
        if k & 1:
            r = add(r, pt)
        pt = add(pt, pt)
        k >>= 1
    return r


def compress(pt):
    x, y = pt
    return bytes([2 + (y & 1)]) + x.to_bytes(32, "big")


def decompress(data):
    prefix, x = data[0], int.from_bytes(data[1:], "big")
    alpha = (x * x * x + A * x + B) % P
    y = pow(alpha, (P + 1) // 4, P)
    if y * y % P != alpha:
        raise ValueError("x is not on the curve")
    if y & 1 != prefix & 1:
        y = P - y
    return (x, y)


def ecdh_x(private: int, peer_public) -> bytes:
    """SEC1 ECDH: the X coordinate only, 32 bytes big-endian."""
    if not on_curve(peer_public):
        raise ValueError("peer public key is not on the curve")
    shared = mul(private, peer_public)
    if shared is None:
        raise ValueError("shared secret is the point at infinity")
    return shared[0].to_bytes(32, "big")
