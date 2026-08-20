#!/usr/bin/env python3
"""Option-4 pairing vectors, generated from the spec in words.

Two ECDH terms: a static-static one that authenticates the hub, and a
static-ephemeral one that supplies freshness. Written from the specification
rather than from either firmware, so that the diff against the device side's
independent generator tests the *specification* and not one side's arithmetic.

Self-checks the curve and the KDF against wire_v3 before emitting anything.
"""
import hashlib
import hmac
import re
import sys

sys.path.insert(0, "tools/pairing")
import p256


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


HUB_EPH_PRIVATE = 0x4b3a291807f6e5d4c3b2a1907e6d5c4b3a2918070605040302010f0e0d0c0b0a


def main():
    v = load("Common/test/vectors/wire_v3.txt")

    # Nothing below is trustworthy unless the curve and the KDF reproduce the
    # numbers wire_v3 already pins. A wrong implementation would otherwise emit
    # plausible vectors, and plausible wrong bytes in a vector file are worse
    # than none - the next reader treats them as settled.
    for who in ("hub", "dev"):
        d = int(v[f"{who}_private"], 16)
        if p256.compress(p256.mul(d, p256.G)).hex() != v[f"{who}_public_compressed"]:
            sys.exit(f"curve does not reproduce {who}_public")
    dev_pub = p256.decompress(bytes.fromhex(v["dev_public_compressed"]))
    if p256.ecdh_x(int(v["hub_private"], 16), dev_pub).hex() != v["ecdh_shared_x_only"]:
        sys.exit("curve does not reproduce ecdh_shared_x_only")
    salt = bytes.fromhex(v["hkdf_salt_hubid_devid_be"])
    if hkdf_sha256(bytes.fromhex(v["ecdh_shared_x_only"]), salt,
                   b"openhub/v1/session", 16).hex() != v["key_session_gen0"]:
        sys.exit("HKDF does not reproduce key_session_gen0")
    print("selfcheck curve + hkdf against wire_v3   ok\n")

    hub_static_d = int(v["hub_private"], 16)
    dev_static_d = int(v["dev_private"], 16)
    hub_static_c = bytes.fromhex(v["hub_public_compressed"])
    dev_static_c = bytes.fromhex(v["dev_public_compressed"])

    hub_eph_pt = p256.mul(HUB_EPH_PRIVATE, p256.G)
    hub_eph_c = p256.compress(hub_eph_pt)

    # Hub before device, in every concatenation. Z1 authenticates the hub - only
    # the holder of hub_static's private key can compute it. Z2 is freshness.
    z1 = p256.ecdh_x(hub_static_d, p256.decompress(dev_static_c))
    z2 = p256.ecdh_x(HUB_EPH_PRIVATE, p256.decompress(dev_static_c))
    assert z1 == p256.ecdh_x(dev_static_d, p256.decompress(hub_static_c))
    assert z2 == p256.ecdh_x(dev_static_d, hub_eph_pt)
    z = z1 + z2

    hub_id, dev_id = 0x33442211, 0x0000002A
    assert salt == hub_id.to_bytes(4, "big") + dev_id.to_bytes(4, "big")

    k_session = hkdf_sha256(z, salt, b"openhub/v1/session", 16)
    k_hop     = hkdf_sha256(z, salt, b"openhub/v1/hop", 16)
    k_cf_hub  = hkdf_sha256(z, salt, b"openhub/v1/confirm/hub", 32)
    k_cf_dev  = hkdf_sha256(z, salt, b"openhub/v1/confirm/dev", 32)

    t = (hub_id.to_bytes(4, "big") + dev_id.to_bytes(4, "big")
         + hub_static_c + hub_eph_c + dev_static_c)

    c_hub = hmac.new(k_cf_hub, t, hashlib.sha256).digest()[:16]
    c_dev = hmac.new(k_cf_dev, t, hashlib.sha256).digest()[:16]

    out = [
        ("pair_hub_eph_public_compressed", hub_eph_c.hex()),
        ("pair_z1", z1.hex()),
        ("pair_z2", z2.hex()),
        ("pair_z", z.hex()),
        ("pair_key_session", k_session.hex()),
        ("pair_key_hop", k_hop.hex()),
        ("pair_confirm_key_hub", k_cf_hub.hex()),
        ("pair_confirm_key_dev", k_cf_dev.hex()),
        ("pair_transcript_len", str(len(t))),
        ("pair_transcript", t.hex()),
        ("pair_transcript_sha256", hashlib.sha256(t).hexdigest()),
        ("pair_confirm_hub", c_hub.hex()),
        ("pair_confirm_dev", c_dev.hex()),
    ]
    for k, val in out:
        print(f"{k:32s} = {val}")

    print()
    print(f"# Z1 != Z2 (a hub reusing its static key as the ephemeral would"
          f" halve Z): {'ok' if z1 != z2 else 'BROKEN'}")
    print(f"# confirmations differ (reflection): {'ok' if c_hub != c_dev else 'BROKEN'}")
    print(f"# key_session differs from wire_v3's single-term value:"
          f" {'ok' if k_session.hex() != v['key_session_gen0'] else 'UNCHANGED - SUSPECT'}")


if __name__ == "__main__":
    main()
