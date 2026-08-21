#!/usr/bin/env python3
"""Parse OpenHub frames out of a demodulated payload, and open the sealed ones.

The layouts here mirror Common/inc/radio_protocol.h. Struct fields are
little-endian on the wire; every crypto input is big-endian. Both live inside a
single frame, which is why the nonce is assembled by hand rather than sliced out
of the header.

Keys are supplied by the operator for debugging. Nothing here derives one.
"""
import struct

# radio_protocol.h
FRAME_JOIN_BEACON = 0x02
FRAME_PAIR_REQ    = 0x03
FRAME_PAIR_RSP    = 0x04
FRAME_PAIR_CONF   = 0x05
FRAME_PAIR_ACCEPT = 0x06
FRAME_UPLINK      = 0x07

DIR_UPLINK   = 0x01
DIR_DOWNLINK = 0x02

UPLINK_AAD_LEN      = 7
PAIR_ACCEPT_AAD_LEN = 15


def nonce(superframe, dev_id, direction, slot):
    """superframe(4) || dev_id(4) || direction(1) || slot(3), all big-endian."""
    return (struct.pack(">II", superframe & 0xFFFFFFFF, dev_id & 0xFFFFFFFF)
            + bytes([direction & 0xFF,
                     (slot >> 16) & 0xFF, (slot >> 8) & 0xFF, slot & 0xFF]))


def _gcm_open(key, iv, aad, ct, tag):
    """AES-128-GCM open. Returns plaintext, or None if the tag does not verify.

    A failed tag is returned as None rather than raised: on a radio bench most
    frames are for someone else, and a wrong key is the normal case.
    """
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        return None
    try:
        return AESGCM(key).decrypt(iv, ct + tag, aad)
    except Exception:
        return None


def parse(payload, keys=None):
    """One line describing the frame, with the body opened if a key fits.

    `payload` is the frame *after* the RFM69 length byte. `keys` maps a name to
    16 raw bytes: "session" opens uplinks, "hop" is reported but not used here.
    """
    keys = keys or {}
    if len(payload) < 2:
        return "short frame (%d bytes)" % len(payload)
    t, ver = payload[0], payload[1]

    if t == FRAME_JOIN_BEACON and len(payload) >= 14:
        net, hub, sf = struct.unpack_from("<HII", payload, 2)
        hops, flags = payload[12], payload[13]
        return ("JOIN_BEACON v%d net=%04x hub=%08x superframe=%u hops=%u flags=%02x"
                % (ver, net, hub, sf, hops, flags))

    if t == FRAME_PAIR_REQ and len(payload) >= 57:
        net, hub, dev, sf = struct.unpack_from("<HIII", payload, 2)
        return ("PAIR_REQ v%d net=%04x hub=%08x dev=%08x superframe=%u "
                "nonce=%s pubkey=%s..."
                % (ver, net, hub, dev, sf,
                   payload[16:24].hex(), payload[24:32].hex()))

    if t == FRAME_UPLINK and len(payload) >= 31:
        slot = payload[2]
        sf, = struct.unpack_from("<I", payload, 3)
        ct, tag = payload[7:15], payload[15:31]
        line = "UPLINK v%d slot=%u superframe=%u ct=%s" % (ver, slot, sf, ct.hex())
        sk = keys.get("session")
        if sk is None:
            return line + "  [sealed - no session key given]"
        # dev_id is not on the wire, so a bench decoder must be told the map.
        # radio_devices_docs/open_hub/testing/sdr.md
        dev = keys.get("dev_id", 0)
        pt = _gcm_open(sk, nonce(sf, dev, DIR_UPLINK, slot),
                       payload[:UPLINK_AAD_LEN], ct, tag)
        if pt is None:
            return line + "  [tag failed - wrong key, or dev_id not %08x]" % dev
        rssi_down = struct.unpack_from("<b", pt, 0)[0]
        flags, supply, uptime = struct.unpack_from("<BHI", pt, 1)
        return ("UPLINK v%d slot=%u superframe=%u  rssi_down=%d dBm flags=%02x "
                "supply=%umV uptime=%us" % (ver, slot, sf, rssi_down, flags,
                                            supply, uptime))

    names = {FRAME_PAIR_RSP: "PAIR_RSP", FRAME_PAIR_CONF: "PAIR_CONF",
             FRAME_PAIR_ACCEPT: "PAIR_ACCEPT"}
    if t in names:
        return "%s v%d %d bytes %s" % (names[t], ver, len(payload),
                                       payload[2:10].hex(" "))
    return "type=0x%02x v%d %d bytes %s" % (t, ver, len(payload),
                                            payload[:12].hex(" "))


def load_keys(path):
    """A small JSON file of hex strings, so keys never sit in shell history."""
    import json
    with open(path) as f:
        raw = json.load(f)
    out = {}
    for k, v in raw.items():
        out[k] = int(v, 0) if k == "dev_id" else bytes.fromhex(v)
    return out
