#!/usr/bin/env python3
"""pair_prov: the same exchange three times, differing only in which superframe
was hashed.

pair_v2 pins the derivation and cannot pin its *source*. It carries one
`pair_req_superframe`, so inside it "the field the device sent", "the last
beacon this side heard" and "the live counter at derive time" are the same
number - and an implementation reading any of the three reproduces the file
forever. Both firmwares passed it while disagreeing on air, which is how a
transcript built over an uninitialised beacon struct survived to the bench.

So this set publishes three superframes that are all simultaneously legitimate:

    prov_req_superframe      what the device put in its PAIR_REQ  <- the answer
    prov_beacon_superframe   the last beacon it heard, necessarily earlier
    prov_live_superframe     the counter at derive time, later - a scalar
                             multiplication costs ~330 ms and the grid moves

Every other input is pair_v2's, byte for byte: same identities, same hub
ephemeral, same device nonce. **Only the superframe varies**, so any difference
between these confirmations and pair_v2's is attributable to that field and to
nothing else.

The two wrong answers are published beside the right one. A set that only
pinned the correct value would report "mismatch" and leave the reader where we
started at one in the morning; pinning what each misread produces makes the
vector name the defect instead of merely refusing it.

**What this does and does not test.** For an implementation where the caller
and the derivation sit together, feeding a request of `prov_req_superframe`
while its own state holds the other two is a real provenance test. For a hub
whose derivation takes the superframe as a parameter, it pins the derivation
and the decoys are documentation - the provenance lives in the wiring from the
received frame to the call, which spans two cores and no host test reaches it.
Saying so here, because a vector that is believed to cover more than it does is
worse than one that covers less.
"""
import hashlib
import hmac
import os
import re
import struct
import sys

sys.path.insert(0, "tools/pairing")
import p256

# pair_v2's, copied rather than imported so a change fails instead of following.
# radio_devices_docs/open_hub/arch/build-and-generation.md
HUB_EPH_PRIVATE = 0x4b3a291807f6e5d4c3b2a1907e6d5c4b3a2918070605040302010f0e0d0c0b0a
DEV_NONCE       = bytes.fromhex("a1b2c3d4e5f60718")
HUB_ID, DEV_ID  = 0x33442211, 0x0000002A

# No zero byte, and no two a byte-permutation of another.
# radio_devices_docs/open_hub/arch/build-and-generation.md
PROV_REQ_SUPERFRAME    = 0x5c3d2e17
PROV_BEACON_SUPERFRAME = 0x5c3d2e11   # earlier: the beacon precedes the request
PROV_LIVE_SUPERFRAME   = 0x5c3d2e19   # later: the derive lands after the grid moved


def hkdf_sha256(secret: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt, secret, hashlib.sha256).digest()
    okm, t, i = b"", b"", 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
        i += 1
    return okm[:length]


def load(path):
    v = {}
    for line in open(path):
        m = re.match(r"\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$", line)
        if m:
            v[m.group(1)] = m.group(2)
    return v


def value_digest(rows):
    """Over the values, sorted by key. Comments are not load-bearing."""
    canon = "\n".join(f"{k}={val}" for k, val in sorted(rows))
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


def derive(z, hub_c, eph_c, dev_c, superframe):
    """One exchange's confirmations, as a function of the superframe alone.

    Everything else is closed over, which is the point: the caller varies one
    argument and the three results are comparable."""
    salt = (struct.pack(">I", HUB_ID) + struct.pack(">I", DEV_ID)
            + struct.pack(">I", superframe) + DEV_NONCE)
    assert len(salt) == 20
    t = (struct.pack(">I", HUB_ID) + struct.pack(">I", DEV_ID)
         + struct.pack(">I", superframe) + DEV_NONCE
         + hub_c + eph_c + dev_c)
    assert len(t) == 119
    k_hub = hkdf_sha256(z, salt, b"openhub/v1/confirm/hub", 32)
    k_dev = hkdf_sha256(z, salt, b"openhub/v1/confirm/dev", 32)
    k_ses = hkdf_sha256(z, salt, b"openhub/v1/session", 16)
    return (salt, t,
            hmac.new(k_hub, t, hashlib.sha256).digest()[:16],
            hmac.new(k_dev, t, hashlib.sha256).digest()[:16],
            k_ses)


def carr(name, data):
    body = ", ".join(f"0x{b:02x}" for b in data)
    wrapped, line = [], "    "
    for tok in body.split(", "):
        if len(line) + len(tok) + 2 > 76:
            wrapped.append(line.rstrip())
            line = "    "
        line += tok + ", "
    wrapped.append(line.rstrip().rstrip(","))
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(wrapped) + "\n};\n"


def main():
    v = load("Common/test/vectors/wire_v3.txt")
    v.update(load("Common/test/vectors/pair_v2.txt"))

    hub_d = int(v["hub_private"], 16)
    dev_d = int(v["dev_private"], 16)
    hub_c = bytes.fromhex(v["hub_public_compressed"])
    dev_c = bytes.fromhex(v["dev_public_compressed"])

    eph_pt = p256.mul(HUB_EPH_PRIVATE, p256.G)
    eph_c  = p256.compress(eph_pt)

    z1 = p256.ecdh_x(hub_d, p256.decompress(dev_c))
    z2 = p256.ecdh_x(HUB_EPH_PRIVATE, p256.decompress(dev_c))
    # Both halves: a one-sided generator agrees with a one-sided error.
    assert z1 == p256.ecdh_x(dev_d, p256.decompress(hub_c))
    assert z2 == p256.ecdh_x(dev_d, eph_pt)
    if z1.hex() != v["pair_z1"]:
        sys.exit("Z1 does not match published pair_z1 - this is not pair_v2's exchange")
    z = z1 + z2

    right = derive(z, hub_c, eph_c, dev_c, PROV_REQ_SUPERFRAME)
    beac  = derive(z, hub_c, eph_c, dev_c, PROV_BEACON_SUPERFRAME)
    live  = derive(z, hub_c, eph_c, dev_c, PROV_LIVE_SUPERFRAME)
    salt, t, c_hub, c_dev, k_ses = right

    # The set is worthless if the three collide; this is the property itself.
    if len({c_hub, beac[2], live[2]}) != 3:
        sys.exit("two sources produce the same confirmation - the set proves nothing")

    # Only the superframe changed, so the confirmation must have moved.
    # radio_devices_docs/open_hub/security/self-tests.md
    if c_hub.hex() == v["pair_confirm_hub"]:
        sys.exit("confirmation unchanged from pair_v2 - the superframe is not bound")
    if t.hex() == v["pair_transcript"]:
        sys.exit("transcript unchanged from pair_v2 - the superframe is not bound")

    # Exactly four bytes at offset 8 and nowhere else.
    # radio_devices_docs/open_hub/arch/build-and-generation.md
    v2t = bytes.fromhex(v["pair_transcript"])
    diff = [i for i in range(119) if v2t[i] != t[i]]
    if diff != [8, 9, 10, 11]:
        sys.exit(f"transcript differs from pair_v2 outside the superframe: {diff}")

    out = [
        ("prov_req_superframe",    f"{PROV_REQ_SUPERFRAME:08x}"),
        ("prov_beacon_superframe", f"{PROV_BEACON_SUPERFRAME:08x}"),
        ("prov_live_superframe",   f"{PROV_LIVE_SUPERFRAME:08x}"),
        ("prov_salt",              salt.hex()),
        ("prov_transcript",        t.hex()),
        ("prov_key_session",       k_ses.hex()),
        ("prov_confirm_hub",       c_hub.hex()),
        ("prov_confirm_dev",       c_dev.hex()),
        ("prov_confirm_hub_if_beacon", beac[2].hex()),
        ("prov_confirm_hub_if_live",   live[2].hex()),
    ]

    digest = value_digest(out)
    lines = ["# pair_prov: one exchange, three candidate superframes.",
             "# Generated by tools/pairing/gen_pair_prov_vectors.py.",
             "# Inputs are pair_v2's byte for byte except the superframe, and the",
             "# transcript is checked to differ from pair_v2's only at offset 8..11.",
             ""]
    for k, val in out:
        lines.append(f"{k:32s} = {val}")
    lines += ["", f"{'prov_value_digest':32s} = {digest}"]
    text = "\n".join(lines) + "\n"

    if "--emit" in sys.argv:
        stem = "Common/test/vectors/pair_prov"
        if os.path.exists(stem + ".txt") and "--force" not in sys.argv:
            if open(stem + ".txt").read() != text:
                sys.exit("refusing to rewrite a published set; emit pair_prov_v2 instead")
        open(stem + ".txt", "w").write(text)
        # Retired: the .txt is the published record and a header can be included
        # by mistake. radio_devices_docs/radio/crypto/wire-crypto.md
        print(f"wrote {stem}.txt and {stem}.h   digest {digest}")
    else:
        print(text)
        print(f"# digest = {digest}   (pass --emit to publish)")


if __name__ == "__main__":
    main()
