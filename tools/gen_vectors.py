#!/usr/bin/env python3
"""Generate the hub/device interop vectors.

Values come from a host reference library, never from either MCU's HAL, so a
board that reproduces them agrees with the specification rather than with
itself. Hub and device share no code - only these bytes - so anything the spec
leaves implicit shows up here as a mismatch instead of in the field.

A published vector set is immutable. The device repository includes the
generated header directly rather than copying it, so rewriting wire_vN in place
would change the contract under a build that has already agreed to it. Changing
any parameter means emitting wire_v(N+1); this script refuses to overwrite an
existing set with different content.

See radio_devices_docs/radio/crypto/wire-crypto.md.
"""

import argparse
import hashlib
import hmac
import os
import sys

from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# Fixed and reproducible; clamped per RFC 7748 5, as a device stores them.
D_HUB = bytes.fromhex("1f2e3d4c5b6a798897a6b5c4d3e2f10012233445566778899aabbccddeeff001")
D_DEV = bytes.fromhex("0a1b2c3d4e5f60718293a4b5c6d7e8f900112233445566778899aabbccddeeff")


def clamp(raw):
    b = bytearray(raw)
    b[0] &= 248
    b[31] = (b[31] & 127) | 64
    return bytes(b)


D_HUB = clamp(D_HUB)
D_DEV = clamp(D_DEV)

HUB_ID = 0x33442211
DEV_ID = 0x0000002A
NET_ID = 0x0001

INFO_SESSION = b"openhub/v1/session"
INFO_HOP     = b"openhub/v1/hop"
INFO_ROTATE  = b"openhub/v1/rotate"

SUPERFRAME = 123456
DIRECTION  = 0x01          # uplink
SLOT       = 7
# A length deliberately not a multiple of four, which the case above cannot be.
# radio_devices_docs/open_hub/security/self-tests.md
SLOT_ODD   = 9
ODD_LEN    = 23


def hkdf(salt, ikm, info, length):
    """RFC 5869 with SHA-256."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


# u = 0 takes every scalar to zero, and a zero shared secret must be refused.
# radio_devices_docs/open_hub/security/self-tests.md
REJECT_U = bytes(32)


def pub_raw(priv):
    """RFC 7748: the u-coordinate alone, 32 bytes little-endian."""
    return priv.public_key().public_bytes_raw()


def build_nonce(superframe, dev_id, direction, slot):
    """Big-endian throughout: the nonce is a crypto input, not a wire field."""
    return (superframe.to_bytes(4, "big")
            + dev_id.to_bytes(4, "big")
            + bytes([direction])
            + slot.to_bytes(3, "big"))


def build_header(frame_type, version, net_id, dev_id):
    """The header as transmitted, so little-endian - this is a wire field."""
    return (bytes([frame_type, version])
            + net_id.to_bytes(2, "little")
            + dev_id.to_bytes(4, "little"))



# The digest covers the pinned values, in a canonical form, and nothing else.
# radio_devices_docs/open_hub/arch/build-and-generation.md
def value_digest(rows):
    body = "".join(f"{k}={v}\n" for k, v in sorted((k, v) for k, v in rows if k and v))
    return hashlib.sha256(body.encode()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", type=int, default=1,
                    help="vector set number; bump it to change any parameter")
    ap.add_argument("--force", action="store_true",
                    help="overwrite a differing set of the same version. Only safe "
                         "if it was never shared - the device build includes it.")
    a = ap.parse_args()

    assert D_HUB == clamp(D_HUB) and D_DEV == clamp(D_DEV), "scalars must be clamped"

    hub = x25519.X25519PrivateKey.from_private_bytes(D_HUB)
    dev = x25519.X25519PrivateKey.from_private_bytes(D_DEV)

    # The shared secret is the u-coordinate, 32 bytes little-endian.
    # radio_devices_docs/radio/crypto/wire-crypto.md
    z_hub = hub.exchange(dev.public_key())
    z_dev = dev.exchange(hub.public_key())
    assert z_hub == z_dev, "ECDH disagreed"
    z = z_hub

    salt = HUB_ID.to_bytes(4, "big") + DEV_ID.to_bytes(4, "big")
    k_session = hkdf(salt, z, INFO_SESSION, 16)
    k_hop     = hkdf(salt, z, INFO_HOP, 16)
    assert k_session != k_hop, "info strings must separate the keys"

    # Daily ratchet, one step. Empty salt: the input is already a key.
    k_session_1 = hkdf(b"", k_session, INFO_ROTATE, 16)

    nonce  = build_nonce(SUPERFRAME, DEV_ID, DIRECTION, SLOT)
    header = build_header(0x01, 0x01, NET_ID, DEV_ID)
    plain  = bytes(range(0x10, 0x10 + 24))
    sealed = AESGCM(k_session).encrypt(nonce, plain, header)
    ct, tag = sealed[:-16], sealed[-16:]

    # Same key, different nonce - never the same pair twice.
    nonce_odd = build_nonce(SUPERFRAME, DEV_ID, DIRECTION, SLOT_ODD)
    plain_odd = bytes(range(0x40, 0x40 + ODD_LEN))
    assert ODD_LEN % 4 != 0, "the point of this case is a partial final word"
    sealed_odd = AESGCM(k_session).encrypt(nonce_odd, plain_odd, header)
    ct_odd, tag_odd = sealed_odd[:-16], sealed_odd[-16:]

    rows = [
        ("# OpenHub wire crypto vectors, X25519. ADR-0025", ""),
        ("# Generated by tools/gen_vectors.py from a host reference library.", ""),
        ("# Hub and device share no code; these bytes are the contract.", ""),
        ("", ""),
        ("# --- ECDH, X25519 (RFC 7748) ---", ""),
        ("# One representation and one width: there is no compressed form to", ""),
        ("# disagree about, no Y to recover, and no point off the curve.", ""),
        ("hub_private", D_HUB.hex()),
        ("dev_private", D_DEV.hex()),
        ("hub_public", pub_raw(hub).hex()),
        ("dev_public", pub_raw(dev).hex()),
        ("", ""),
        ("# Must be REJECTED. u=0 has order 1, so every scalar sends it to zero;", ""),
        ("# X25519 returns that zero rather than failing, and using it is the bug.", ""),
        ("reject_low_order_u", REJECT_U.hex()),
        ("ecdh_shared_u_only", z.hex()),
        ("", ""),
        ("# --- HKDF-SHA256 at pairing ---", ""),
        ("hkdf_salt_hubid_devid_be", salt.hex()),
        ("info_session", INFO_SESSION.decode()),
        ("info_hop", INFO_HOP.decode()),
        ("key_session_gen0", k_session.hex()),
        ("key_hop_gen0", k_hop.hex()),
        ("", ""),
        ("# --- daily ratchet, one step, empty salt ---", ""),
        ("info_rotate", INFO_ROTATE.decode()),
        ("key_session_gen1", k_session_1.hex()),
        ("", ""),
        ("# --- AES-128-GCM, frame shaped ---", ""),
        (f"# nonce = superframe({SUPERFRAME}) || dev_id({DEV_ID}) || dir({DIRECTION}) || slot({SLOT}), all big-endian", ""),
        ("# aad   = frame header as transmitted, so little-endian fields", ""),
        ("gcm_key", k_session.hex()),
        ("gcm_nonce", nonce.hex()),
        ("gcm_aad", header.hex()),
        ("gcm_plaintext", plain.hex()),
        ("gcm_ciphertext", ct.hex()),
        ("gcm_tag", tag.hex()),
        ("", ""),
        (f"# --- AES-128-GCM, {ODD_LEN}-byte payload: a partial final word ---", ""),
        ("# Block-aligned cases cannot catch a decrypt path that fails to mask", ""),
        ("# the unused bytes of the last word. Real frames are not 4-byte multiples.", ""),
        ("gcm_odd_nonce", nonce_odd.hex()),
        ("gcm_odd_plaintext", plain_odd.hex()),
        ("gcm_odd_ciphertext", ct_odd.hex()),
        ("gcm_odd_tag", tag_odd.hex()),
    ]

    lines = []
    for label, value in rows:
        if not label:
            lines.append("")
        elif not value:
            lines.append(label)
        else:
            lines.append(f"{label} = {value}")

    text = "\n".join(lines) + "\n"
    digest = value_digest(rows)

    stem = f"Common/test/vectors/wire_v{a.version}"
    if os.path.exists(stem + ".txt") and not a.force:
        old = open(stem + ".txt").read()
        if old != text:
            sys.exit(
                f"refusing to rewrite {stem}.txt with different content.\n"
                f"These bytes are a published contract and the device build\n"
                f"includes the generated header directly. Emit a new set with\n"
                f"  --version {a.version + 1}\n"
                f"and tell the device side, or pass --force if this set was\n"
                f"never shared.")

    with open(stem + ".txt", "w") as f:
        f.write(text)

    # The same bytes as C, emitted rather than transcribed.
    # radio_devices_docs/open_hub/arch/build-and-generation.md
    def carr(name, data):
        body = ", ".join(f"0x{b:02x}" for b in data)
        wrapped, line = [], "    "
        for tok in body.split(", "):
            if len(line) + len(tok) + 2 > 76:
                wrapped.append(line.rstrip())
                line = "    "
            line += tok + ", "
        wrapped.append(line.rstrip().rstrip(","))
        joined = "\n".join(wrapped)
        return f"static const uint8_t {name}[{len(data)}] = {{\n{joined}\n}};\n"

    h = ["/* Generated by tools/gen_vectors.py - do not edit. */",
         "/* Immutable once published: the device build includes this file. */",
         "#pragma once", "", "#include <stdint.h>", "",
         f"#define WIRE_VECTORS_VERSION {a.version}",
         f'#define WIRE_VECTORS_DIGEST  "{digest}"',
         ""]
    h.append(carr("V_HUB_PRIV", D_HUB))
    # A published row that was never emitted as C, so nothing consumed it.
    h.append(carr("V_DEV_PRIV", D_DEV))
    h.append(carr("V_DEV_PUB", pub_raw(dev)))
    h.append(carr("V_ECDH_SHARED", z))
    h.append(carr("V_SALT", salt))
    h.append(carr("V_INFO_SESSION", INFO_SESSION))
    h.append(carr("V_INFO_HOP", INFO_HOP))
    h.append(carr("V_INFO_ROTATE", INFO_ROTATE))
    h.append(carr("V_KEY_SESSION0", k_session))
    h.append(carr("V_KEY_HOP0", k_hop))
    h.append(carr("V_KEY_SESSION1", k_session_1))
    h.append(carr("V_NONCE", nonce))
    h.append(carr("V_AAD", header))
    h.append(carr("V_PLAIN", plain))
    h.append(carr("V_CIPHER", ct))
    h.append(carr("V_TAG", tag))
    h.append(carr("V_ODD_NONCE", nonce_odd))
    h.append(carr("V_ODD_PLAIN", plain_odd))
    h.append(carr("V_ODD_CIPHER", ct_odd))
    h.append(carr("V_ODD_TAG", tag_odd))
    h.append(carr("V_HUB_PUB", pub_raw(hub)))
    h.append(carr("V_REJECT_U", REJECT_U))
    with open(stem + ".h", "w") as f:
        f.write("\n".join(h))

    print(text)
    print(f"# set v{a.version}, digest {digest[:16]}")


if __name__ == "__main__":
    main()
