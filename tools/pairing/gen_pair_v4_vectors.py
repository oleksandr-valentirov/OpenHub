#!/usr/bin/env python3
"""pair_v4: the same exchange over X25519, and an invitation that carries a key.

Two changes from pair_v2, and they are independent of each other.

ADR-0025 replaces P-256 with X25519, so every point is 32 bytes and there is one
representation of it. The transcript shrinks from 119 bytes to 116 and the two
frames that carry a point shrink with it. The derivation itself does not move:
the same salt, the same info strings, the same two-term Z with the hub's static
term first.

ADR-0024 makes the device id the whole enrolment anchor, so PAIR_INIT now
carries the hub's static key in the clear and has no MAC to carry: under mode
OPEN the field is present and zero. pair_v3's invitation key does not exist any
more and no replacement is emitted, because nothing derives one.

Written from the specification in words rather than from either firmware, so a
diff against the device side's own reading tests the specification.
"""
import hashlib
import hmac
import os
import re
import struct
import sys

from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# --- inputs, pinned so the two generators can be diffed at all ---------------
HUB_EPH_PRIVATE = bytes.fromhex(
    "4b3a291807f6e5d4c3b2a1907e6d5c4b3a2918070605040302010f0e0d0c0b0a")
DEV_NONCE       = bytes.fromhex("a1b2c3d4e5f60718")
REQ_SUPERFRAME  = 0x1a2b3c4d   # no zero byte: a byte-order slip cannot land plausibly
NET_ID          = 0x0001
VERSION         = 3            # RADIO_PROTO_VERSION, moved by ADR-0025
INIT_VERSION    = 4            # RADIO_PAIR_INIT_VERSION, moved by ADR-0024
ENROL_MODE_OPEN = 0x00

# The invitation names its own superframe, ahead of the request it provokes.
INIT_SUPERFRAME = 0x1a2b3c49

# PAIR_ACCEPT goes out later in the exchange than the request it answers.
ACCEPT_SUPERFRAME = 0x1a2b3c4f
ACCEPT_RETRY      = 0
GRANT_SLOT         = 0
GRANT_REPORT_EVERY = 8
GRANT_FLAGS        = 0
# Random per hub in the field. Fixed here so the frame bytes are reproducible.
NET_HOP_KEY = bytes(range(16))

UPLINK_SUPERFRAME = 0x1a2b3c58
UPLINK_SLOT       = 0
RPT_RSSI_DOWN = -92
RPT_FLAGS     = 0
RPT_SUPPLY_MV = 3287
RPT_UPTIME_S  = 61

DIR_UPLINK   = 0x01
DIR_DOWNLINK = 0x02
NONCE_SLOT_UNSLOTTED = 0xFFFF00


def clamp(raw):
    b = bytearray(raw)
    b[0] &= 248
    b[31] = (b[31] & 127) | 64
    return bytes(b)


def value_digest(rows):
    body = "".join(f"{k}={v}\n" for k, v in sorted(rows))
    return hashlib.sha256(body.encode()).hexdigest()[:16]


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


def nonce(superframe: int, dev_id: int, direction: int, slot: int) -> bytes:
    """The 12-byte GCM nonce. Big-endian throughout, unlike the frame fields."""
    return (struct.pack(">I", superframe) + struct.pack(">I", dev_id)
            + bytes([direction]) + struct.pack(">I", slot)[1:])


def priv(raw_hex):
    return x25519.X25519PrivateKey.from_private_bytes(bytes.fromhex(raw_hex))


def pub_of(sk):
    return sk.public_key().public_bytes_raw()


def selfcheck(v):
    """Against wire_v4, so this generator's curve is not its own witness."""
    for who in ("hub", "dev"):
        if pub_of(priv(v[f"{who}_private"])).hex() != v[f"{who}_public"]:
            sys.exit(f"curve does not reproduce {who}_public")
    z = priv(v["hub_private"]).exchange(
        x25519.X25519PublicKey.from_public_bytes(bytes.fromhex(v["dev_public"])))
    if z.hex() != v["ecdh_shared_u_only"]:
        sys.exit("curve does not reproduce ecdh_shared_u_only")
    salt_v1 = bytes.fromhex(v["hkdf_salt_hubid_devid_be"])
    if hkdf_sha256(z, salt_v1, b"openhub/v1/session", 16).hex() != v["key_session_gen0"]:
        sys.exit("HKDF does not reproduce key_session_gen0")

    ct = AESGCM(bytes.fromhex(v["key_session_gen0"])).encrypt(
        bytes.fromhex(v["gcm_nonce"]), bytes.fromhex(v["gcm_plaintext"]),
        bytes.fromhex(v["gcm_aad"]))
    if ct.hex() != v["gcm_ciphertext"] + v["gcm_tag"]:
        sys.exit("AES-GCM does not reproduce wire_v4's sealed frame")
    print("selfcheck curve + hkdf + gcm against wire_v4   ok\n")


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


def emit_header(rows, digest):
    d = dict(rows)
    h = ["/* Generated by tools/pairing/gen_pair_v4_vectors.py - do not edit. */",
         "/* Immutable once published: both firmwares compile against this. */",
         "#pragma once", "", "#include <stdint.h>", "",
         "#define PAIR_VECTORS_VERSION 4",
         f'#define PAIR_VECTORS_DIGEST  "{digest}"',
         f'#define PAIR_REQ_SUPERFRAME  0x{d["pair_req_superframe"]}u',
         f'#define PAIR_INIT_SUPERFRAME 0x{d["pair_init_superframe"]}u',
         ""]
    for name, key in [
            ("PV_DEV_NONCE", "pair_dev_nonce"),
            ("PV_HUB_STATIC", "pair_hub_static"),
            ("PV_DEV_STATIC", "pair_dev_static"),
            ("PV_HUB_EPH_PRIV", "pair_hub_eph_private"),
            ("PV_HUB_EPH_PUB", "pair_hub_eph_public"),
            ("PV_FINGERPRINT", "pair_fingerprint"),
            ("PV_Z", "pair_z"),
            ("PV_SALT", "pair_salt"),
            ("PV_KEY_SESSION", "pair_key_session"),
            ("PV_KEY_ROOT", "pair_key_root"),
            ("PV_CONFIRM_KEY_HUB", "pair_confirm_key_hub"),
            ("PV_CONFIRM_KEY_DEV", "pair_confirm_key_dev"),
            ("PV_TRANSCRIPT", "pair_transcript"),
            ("PV_CONFIRM_HUB", "pair_confirm_hub"),
            ("PV_CONFIRM_DEV", "pair_confirm_dev"),
            ("PV_NET_HOP_KEY", "pair_net_hop_key"),
            ("PV_ACCEPT_NONCE", "pair_accept_nonce"),
            ("PV_ACCEPT_AAD", "pair_accept_aad"),
            ("PV_ACCEPT_PLAIN", "pair_accept_plaintext"),
            ("PV_FRAME_INIT", "frame_pair_init"),
            ("PV_FRAME_REQ", "frame_pair_req"),
            ("PV_FRAME_RSP", "frame_pair_rsp"),
            ("PV_FRAME_CONF", "frame_pair_conf"),
            ("PV_FRAME_ACCEPT", "frame_pair_accept"),
            ("PV_UPLINK_NONCE", "uplink_nonce"),
            ("PV_UPLINK_AAD", "uplink_aad"),
            ("PV_UPLINK_PLAIN", "uplink_plaintext"),
            ("PV_FRAME_UPLINK", "frame_uplink")]:
        h.append(carr(name, bytes.fromhex(d[key])))
    return "\n".join(h)


def main():
    v = load("Common/test/vectors/wire_v4.txt")
    selfcheck(v)

    hub_sk = priv(v["hub_private"])
    dev_sk = priv(v["dev_private"])
    hub_static = pub_of(hub_sk)
    dev_static = pub_of(dev_sk)
    hub_id, dev_id = 0x33442211, 0x0000002A

    eph_sk = x25519.X25519PrivateKey.from_private_bytes(clamp(HUB_EPH_PRIVATE))
    hub_eph = pub_of(eph_sk)

    # Hub term first in every concatenation: Z1 authenticates, Z2 is freshness.
    z1 = hub_sk.exchange(x25519.X25519PublicKey.from_public_bytes(dev_static))
    z2 = eph_sk.exchange(x25519.X25519PublicKey.from_public_bytes(dev_static))
    assert z1 == dev_sk.exchange(
        x25519.X25519PublicKey.from_public_bytes(hub_static))
    assert z2 == dev_sk.exchange(
        x25519.X25519PublicKey.from_public_bytes(hub_eph))
    assert z1.hex() == v["ecdh_shared_u_only"], "Z1 is still wire_v4's ECDH"
    assert z1 != z2, "a static ephemeral would collapse the two terms"
    z = z1 + z2

    # 20 bytes: the two identities, then both contributions of freshness.
    salt = (struct.pack(">I", hub_id) + struct.pack(">I", dev_id)
            + struct.pack(">I", REQ_SUPERFRAME) + DEV_NONCE)
    assert len(salt) == 20

    k_session = hkdf_sha256(z, salt, b"openhub/v1/session", 16)
    k_root    = hkdf_sha256(z, salt, b"openhub/v1/root", 32)
    k_cf_hub  = hkdf_sha256(z, salt, b"openhub/v1/confirm/hub", 32)
    k_cf_dev  = hkdf_sha256(z, salt, b"openhub/v1/confirm/dev", 32)

    t = (struct.pack(">I", hub_id) + struct.pack(">I", dev_id)
         + struct.pack(">I", REQ_SUPERFRAME) + DEV_NONCE
         + hub_static + hub_eph + dev_static)
    assert len(t) == 116

    c_hub = hmac.new(k_cf_hub, t, hashlib.sha256).digest()[:16]
    c_dev = hmac.new(k_cf_dev, t, hashlib.sha256).digest()[:16]

    fingerprint = hashlib.sha256(dev_static).digest()

    # --- the frames, byte for byte -----------------------------------------

    # Struct fields are little-endian and the crypto inputs above big-endian.
    # radio_devices_docs/radio/crypto/wire-crypto.md
    init = (struct.pack("<BBHIII", 0x09, INIT_VERSION, NET_ID, hub_id, dev_id,
                        INIT_SUPERFRAME)
            + bytes([ENROL_MODE_OPEN]) + hub_static + bytes(12))
    assert len(init) == 61
    # The MAC field is present and zero: mode OPEN has no secret to key it with.
    assert init[-12:] == bytes(12)

    req = (struct.pack("<BBHIII", 0x03, VERSION, NET_ID, hub_id, dev_id,
                       REQ_SUPERFRAME) + DEV_NONCE + dev_static)
    assert len(req) == 56
    rsp = struct.pack("<BBII", 0x04, VERSION, hub_id, dev_id) + hub_eph + c_hub
    assert len(rsp) == 58
    conf = struct.pack("<BBII", 0x05, VERSION, hub_id, dev_id) + c_dev
    assert len(conf) == 26

    grant = bytes([GRANT_SLOT, GRANT_REPORT_EVERY, GRANT_FLAGS]) + NET_HOP_KEY
    assert len(grant) == 19
    acc_hdr = struct.pack("<BBIIIB", 0x06, VERSION, hub_id, dev_id,
                          ACCEPT_SUPERFRAME, ACCEPT_RETRY)
    assert len(acc_hdr) == 15
    acc_nonce = nonce(ACCEPT_SUPERFRAME, dev_id, DIR_DOWNLINK,
                      NONCE_SLOT_UNSLOTTED | ACCEPT_RETRY)
    sealed = AESGCM(k_session).encrypt(acc_nonce, grant, acc_hdr)
    accept = acc_hdr + sealed
    assert len(accept) == 50

    report = struct.pack("<bBHI", RPT_RSSI_DOWN, RPT_FLAGS, RPT_SUPPLY_MV,
                         RPT_UPTIME_S)
    assert len(report) == 8
    up_hdr = struct.pack("<BBBI", 0x07, VERSION, UPLINK_SLOT, UPLINK_SUPERFRAME)
    assert len(up_hdr) == 7
    up_nonce = nonce(UPLINK_SUPERFRAME, dev_id, DIR_UPLINK, UPLINK_SLOT)
    uplink = up_hdr + AESGCM(k_session).encrypt(up_nonce, report, up_hdr)
    assert len(uplink) == 31

    out = [
        ("pair_dev_nonce", DEV_NONCE.hex()),
        ("pair_req_superframe", "%08x" % REQ_SUPERFRAME),
        ("pair_init_superframe", "%08x" % INIT_SUPERFRAME),
        ("pair_hub_static", hub_static.hex()),
        ("pair_dev_static", dev_static.hex()),
        ("pair_hub_eph_private", clamp(HUB_EPH_PRIVATE).hex()),
        ("pair_hub_eph_public", hub_eph.hex()),
        ("pair_fingerprint_domain", "x25519-u-32"),
        ("pair_fingerprint", fingerprint.hex()),
        ("pair_z1", z1.hex()),
        ("pair_z2", z2.hex()),
        ("pair_z", z.hex()),
        ("pair_salt_len", str(len(salt))),
        ("pair_salt", salt.hex()),
        ("pair_key_session", k_session.hex()),
        ("pair_key_root", k_root.hex()),
        ("pair_confirm_key_hub", k_cf_hub.hex()),
        ("pair_confirm_key_dev", k_cf_dev.hex()),
        ("pair_transcript_len", str(len(t))),
        ("pair_transcript", t.hex()),
        ("pair_transcript_sha256", hashlib.sha256(t).hexdigest()),
        ("pair_confirm_hub", c_hub.hex()),
        ("pair_confirm_dev", c_dev.hex()),
        ("pair_enrol_mode", "%02x" % ENROL_MODE_OPEN),
        ("frame_pair_init", init.hex()),
        ("frame_pair_req", req.hex()),
        ("frame_pair_rsp", rsp.hex()),
        ("frame_pair_conf", conf.hex()),
        ("pair_net_hop_key", NET_HOP_KEY.hex()),
        ("pair_accept_superframe", "%08x" % ACCEPT_SUPERFRAME),
        ("pair_accept_nonce", acc_nonce.hex()),
        ("pair_accept_aad", acc_hdr.hex()),
        ("pair_accept_plaintext", grant.hex()),
        ("frame_pair_accept", accept.hex()),
        ("uplink_superframe", "%08x" % UPLINK_SUPERFRAME),
        ("uplink_nonce", up_nonce.hex()),
        ("uplink_aad", up_hdr.hex()),
        ("uplink_plaintext", report.hex()),
        ("frame_uplink", uplink.hex()),
    ]
    header = """# OpenHub pairing vectors, v4
# Generated by tools/pairing/gen_pair_v4_vectors.py from the spec in words.
# Immutable once published.
#
# SUPERSEDES pair_v2 and pair_v3. Two decisions land here at once and they are
# independent: ADR-0025 moves the curve to X25519, and ADR-0024 makes the device
# id the whole enrolment anchor.
#
# The derivation did NOT move. Same salt, same info strings, same two-term Z
# with the hub's static term first. What moved is the width of a point - 33
# bytes to 32 - so the transcript is 116 rather than 119 and the two frames
# carrying a point each lost a byte. A port that changed the KDF as well would
# reproduce nothing here and would look like a curve problem.
#
# NOT DERIVED, deliberately: an invitation key. pair_v3 published one because
# PAIR_INIT was MACed under a key both ends could compute from their static
# keys. Under ADR-0024 the hub has no device key at invitation time, so there is
# nothing to derive and the MAC field is present and zero. frame_pair_init pins
# that: mode 00, the hub's static key in the clear, twelve zero bytes.
# Publishing a key with no consumer is the shape that let pair_v1's defect live.
#
# Field endianness: the crypto inputs below - salt, transcript, nonce - are
# big-endian. The frame_* bytes are the frames as transmitted, and their struct
# fields are LITTLE-endian. X25519 scalars and u-coordinates are little-endian
# by RFC 7748 and are not byte-swapped for the wire.
"""

    lines = [header]
    for k, val in out:
        lines.append(f"{k:32s} = {val}")
    text = "\n".join(lines) + "\n"
    digest = value_digest(out)

    if "--emit" in sys.argv:
        stem = "Common/test/vectors/pair_v4"
        if os.path.exists(stem + ".txt") and "--force" not in sys.argv:
            if open(stem + ".txt").read() != text:
                sys.exit("refusing to rewrite a published set; emit pair_v5 instead")
        open(stem + ".txt", "w").write(text)
        open(stem + ".h", "w").write(emit_header(out, digest))
        print(f"wrote {stem}.txt and {stem}.h   digest {digest}")
    else:
        print(text)
        print(f"# digest = {digest}   (pass --emit to publish)")

    print()
    # A port that dropped the freshness would reproduce pair_v2's key exactly.
    v2 = load("Common/test/vectors/pair_v2.txt")
    print("# key_session differs from pair_v2 (the curve reached the KDF):",
          "ok" if k_session.hex() != v2["pair_key_session"] else "UNCHANGED - SUSPECT")
    print("# transcript is 116 bytes, was 119:",
          "ok" if len(t) == 116 and len(v2["pair_transcript"]) // 2 == 119 else "SUSPECT")


if __name__ == "__main__":
    main()
