#!/usr/bin/env python3
"""Candidate vectors for the pairing exchange, generated from the spec in words.

Deliberately implemented from the written specification and not from either
firmware, because the point of the exercise is to test the *specification*. A
generator that calls one side's code confirms that side's understanding and
learns nothing - which is how the hop sequence's three divergences were found,
and how they would have been missed.

Needs nothing but hashlib: wire_v3 already pins the ECDH output, so no curve
arithmetic happens here.
"""
import hashlib
import hmac
import re
import sys


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


def main():
    v = load("Common/test/vectors/wire_v3.txt")

    secret = bytes.fromhex(v["ecdh_shared_x_only"])
    salt = bytes.fromhex(v["hkdf_salt_hubid_devid_be"])

    # Confirm the tool before trusting it with anything new.
    # radio_devices_docs/open_hub/arch/build-and-generation.md
    for info, want in ((b"openhub/v1/session", v["key_session_gen0"]),
                       (b"openhub/v1/hop", v["key_hop_gen0"])):
        got = hkdf_sha256(secret, salt, info, 16).hex()
        status = "ok" if got == want else "MISMATCH"
        print(f"selfcheck {info.decode():24s} {got}  {status}")
        if got != want:
            sys.exit("HKDF does not reproduce wire_v3; nothing below is trustworthy")
    print()

    dev_pub_c = bytes.fromhex(v["dev_public_compressed"])
    hub_pub_c = bytes.fromhex(v["hub_public_compressed"])

    # Ids as wire_v3's salt already spells them: hub 0x33442211, dev 0x2a.
    hub_id = 0x33442211
    dev_id = 0x0000002A
    assert salt == hub_id.to_bytes(4, "big") + dev_id.to_bytes(4, "big")

    fingerprint = hashlib.sha256(dev_pub_c).digest()

    # Four named byte strings, hub_id first to match the HKDF salt.
    # radio_devices_docs/radio/pairing.md
    transcript = (hub_id.to_bytes(4, "big")
                  + dev_id.to_bytes(4, "big")
                  + dev_pub_c
                  + hub_pub_c)

    k_hub = hkdf_sha256(secret, salt, b"openhub/v1/confirm/hub", 32)
    k_dev = hkdf_sha256(secret, salt, b"openhub/v1/confirm/dev", 32)
    c_hub = hmac.new(k_hub, transcript, hashlib.sha256).digest()[:16]
    c_dev = hmac.new(k_dev, transcript, hashlib.sha256).digest()[:16]

    print(f"pair_fingerprint_domain    = compressed-sec1-33")
    print(f"pair_fingerprint           = {fingerprint.hex()}")
    print(f"pair_fingerprint_display   = {fingerprint.hex()[:12].upper()}")
    print()
    print(f"pair_transcript_len        = {len(transcript)}")
    print(f"pair_transcript            = {transcript.hex()}")
    # A diagnostic, not a protocol value: the HMAC is over the transcript itself.
    # radio_devices_docs/radio/pairing.md
    print(f"pair_transcript_sha256     = {hashlib.sha256(transcript).hexdigest()}")
    print()
    print(f"pair_confirm_key_hub       = {k_hub.hex()}")
    print(f"pair_confirm_key_dev       = {k_dev.hex()}")
    print(f"pair_confirm_hub           = {c_hub.hex()}")
    print(f"pair_confirm_dev           = {c_dev.hex()}")
    print()
    print("# reflection check: the two confirmations must not be equal, or one")
    print("# side's value could be replayed back at it")
    print(f"pair_confirm_differ        = {'yes' if c_hub != c_dev else 'NO - BROKEN'}")


if __name__ == "__main__":
    main()
