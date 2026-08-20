# Wire crypto format

**Status: frozen, and the device depends on it.** Vector sets are immutable once
published; a parameter change emits the next set rather than rewriting one.

| Set | Covers |
|---|---|
| `wire_v3` | AES-GCM, HKDF, compressed points, **single-term** ECDH |
| `pair_v1` | what the **pairing exchange** produces |

**`wire_v3`'s `key_session_gen0` and `key_hop_gen0` are not pairing outputs any
more.** Pairing derives from a 64-byte `Z` with two ECDH terms, so those values
are the single-term case and nothing else. They are still correct for what they
now name, and `wire_v3`'s `ecdh_shared_x_only` is exercised directly — it is
`pair_v1`'s `pair_z1`.

Recorded prominently because the hazard is not a reader mistaking old numbers for
new ones. It is that a self-test **passes** while asserting a contract that has
moved: green, and naming the wrong thing. The hub's case is now called
"wire vectors (primitives)" for that reason.

`pair_v1` was cross-verified before publication: two implementations written from
the specification in words, neither reading the other's code, produced all eleven
values identically — including the ephemeral public key, so the two independent
P-256 implementations agree as well.

This page is the contract between hub and device. They share no code
([ADR-0012](../decisions/0012-wire-format-is-the-contract.md)): the hub uses
mbedTLS with software P-256, the device uses PKA with software SHA-256. Two
independent implementations only interoperate if the bytes are specified, so
nothing here may be left as "whatever the library does".

## Primitives

| Role | Algorithm | Hub | Device |
|---|---|---|---|
| Key agreement | ECDH on **NIST P-256** (secp256r1) | mbedTLS, software | PKA hardware |
| Key derivation | HKDF-SHA256 (RFC 5869) | HASH hardware | software SHA-256 |
| Frame protection | **AES-128-GCM**, 16-byte tag | CRYP hardware | AES hardware |

P-256 over X25519 is a deliberate choice driven by the device side having hardware
for it and none for anything else — [ADR-0010](../decisions/0010-p256-over-x25519.md).

## Byte-level rules

These are the details that silently break interoperability if left implicit.

**Public keys** travel as the **compressed SEC1 point**: `0x02` or `0x03`
followed by X big-endian, fixed width, zero-padded — 33 bytes.

This reverses an earlier choice of uncompressed points, which weighed device
cycles and forgot the hub's radio: **the RFM69 FIFO is 66 bytes**, so a 65-byte
key plus any header cannot be sent in one load. Recovering Y is one modular
exponentiation. See [ADR-0018](../decisions/0018-compressed-points-on-the-wire.md) —
**Accepted** — the device measured decompression at **13 ms** on the PKA against
**103 ms** for the scalar multiply that follows. It buys 32 bytes, which at
25 kbps is 10.2 ms of air, and air is the scarce resource under a 1% duty cycle
while the device core is otherwise ~99% idle.

**The ECDH shared secret** is the **X coordinate only**, 32 bytes big-endian, as
in SEC1 ECDH — *not* the full point and *not* a hash of it. This is the single
most common interoperability mistake between two hand-built implementations.

**Every received public key must be validated** before use: on the curve, not the
point at infinity, X in range. An unvalidated point is an invalid-curve attack.

This is **not** made redundant by compression on the hub. mbedTLS does not verify
the square root it extracts, so reading a compressed point whose X is not a valid
x-coordinate **succeeds and yields a garbage Y**. `mbedtls_ecp_check_pubkey()` is
the check that catches it. `wire_v3`'s `reject_compressed_nonresidue` exists to
prove that path is wired — and note that a perturbed valid key would not test it,
since about half of all field elements are valid x-coordinates.

**HKDF** uses SHA-256 with a salt of the two device identifiers and an `info`
string that names the key's purpose, so the session key and the hop key are
different keys derived from one secret and can never be confused for each other.

**Endianness.** The rule, stated so it has no exceptions:

> **Little-endian applies to fields as they appear on the wire. Anything fed into
> the crypto layer is big-endian.**

So `radio_protocol.h` structures stay little-endian, matching the natural layout
on both Cortex-M parts, and every cryptographic value — SEC1 points, the shared
secret, HKDF inputs, **and the nonce** — is big-endian.

The nonce is the case that decides the wording. It is assembled from protocol
counters, so "protocol fields are little-endian" would put *two conventions
inside one 12-byte value* — at the one point where a mismatch is undetectable,
because the nonce is never transmitted and a wrong one only shows up as a tag
that no other implementation reproduces.

It also matters at the hardware. On both parts the key and IV registers bypass
the CRYP `DATATYPE` byte-swapping and must be packed big-endian into words by
hand, while payload and AAD go through as plain bytes. A nonce defined
big-endian end to end is a straight copy into those registers with no per-field
reasoning; a mixed-endian one is not.

## Nonce construction

GCM's one absolute requirement: **a key/nonce pair must never repeat.** A repeat
does not merely leak plaintext, it leaks the authentication subkey and lets an
attacker forge frames.

The nonce is built from values both ends already agree on rather than
transmitted — it costs no air time and cannot be manipulated in flight:

```
  12-byte nonce = superframe counter (4) || device id (4) || direction (1) || slot (3)
```

**All four fields big-endian**, per the rule above.

- **superframe counter** — monotonic, and already broadcast for time alignment
- **device id** — separates devices sharing a hub
- **direction** — uplink and downlink must never collide
- **slot** — separates multiple frames within one superframe

The counter is 32 bits and increments about every 2 seconds, so it wraps after
roughly **272 years**. It is not a practical constraint, but the rotation schedule
gives a fresh key daily regardless, which bounds the exposure of any single key
independently of the counter.

**A device must reject a frame whose counter is not ahead of the last one it
accepted**, within a window that tolerates missed superframes. Without that check
the nonce scheme is sound and replay still works.

## Associated data

The frame header is authenticated but not encrypted — it has to be readable to be
routed. It goes into GCM as AAD, so a modified header fails the tag check rather
than being silently accepted.

**Device ids are assigned with mixed bits, not sequentially.** A small id like
`0x0000002A` puts a run of zero bytes into a plaintext header, and with
[whitening off](../radio/phy.md) nothing breaks that run up before the bit
slicer sees it. The ids are ours to choose, so the problem is removed by
assignment rather than by a coding layer whose two implementations disagree:
**draw them from the RNG** and reject any value whose bytes are mostly equal.

This costs nothing — there are 2^32 ids for at most 64 devices — and it is the
first remedy to reach for if the plaintext header ever causes sync trouble.

## Frame layout

```
  +--------+-------------------+------------------+--------+
  | type   | header (AAD)      | ciphertext       | tag    |
  | 1 byte | ...               | n bytes          | 16 B   |
  +--------+-------------------+------------------+--------+
```

The 16-byte tag is a real cost inside a 1% duty cycle
([radio/phy.md](../radio/phy.md)). Truncating it was considered and **rejected**:
a shortened tag weakens forgery resistance, and forgery resistance is the property
that makes the sensor data trustworthy rather than merely private.

The join beacon is outside all of this — it is cleartext by necessity. See
[radio/joining.md](../radio/joining.md).

## Test vectors

**Written and passing on the hub.** `Common/test/vectors/wire_v3.txt` is the
current set, held as labelled hex; `wire_v2.h` is the same bytes as C, emitted by
the same script rather than transcribed. `wire_v1` remains on disk unchanged.

```bash
.venv/bin/python tools/gen_vectors.py
```

The values come from a **host reference library, never from either MCU's HAL**.
That distinction is the whole point: two implementations agreeing with each other
proves nothing, and agreeing with an independent reference proves they agree with
the specification.

They cover the three things that break silently between independent
implementations:

| Vector | Catches |
|---|---|
| `ecdh_shared_x_only` | using the full point, or a hash of it, instead of X alone |
| `key_session_gen0` / `key_hop_gen0` | one key reused for two purposes |
| `gcm_nonce` / `gcm_ciphertext` / `gcm_tag` | the nonce assembly, not just the cipher |
| `gcm_odd_*` (23 bytes) | a decrypt path that leaves a partial final word unmasked |

The GCM case is deliberately frame-shaped: a big-endian nonce built by the real
construction, and the transmitted (little-endian) header as AAD — so it exercises
the endianness rule end to end.

**v2 exists because v1's 24-byte payload was block-aligned and could not fail.**
The device side found `HAL_CRYP_Decrypt` in GCM mode leaving the unused bytes of
a partial final word unmasked while the encrypt path handled them correctly:
every length not divisible by four failed its tag check with byte-perfect
ciphertext. Over the air that reads as a radio fault, not a crypto one. Both
sides' vector checks passed throughout, because 24 is a multiple of four and real
frames are not conveniently sized. The 23-byte case is checked in **both**
directions, since only decrypt was affected.

Verified on the hub with the `crypto` console command: 5/5, the vector check
taking 245 ms. Verified independently on the device side, 7/7 on both boards —
so two implementations sharing no code reproduce the same third-party bytes.
That, rather than either side's own test suite, is what de-risks first contact.

### A published vector set is immutable

The device repository **includes `wire_vN.h` from this tree rather than copying
it**, so its build fails the moment these bytes change. That is the behaviour
both sides want — a stale copy of a wire contract goes wrong on air, months
later, instead of at compile time.

It also means **rewriting a published set in place is a breaking change**.
Changing any parameter emits the next set instead:

```bash
.venv/bin/python tools/gen_vectors.py --version 2
```

`gen_vectors.py` refuses to overwrite an existing set with different content and
says so, rather than relying on anyone remembering. `--force` exists for a set
that was never shared, and is wrong otherwise.

Each header carries a `*_VERSION` and a `*_DIGEST`, and the `vectors` console
command reads them: it prints what **each core** was built against and says
`MISMATCH` when they differ. That matters because the two cores are flashed
separately and this project has already been bitten by flashing one and not the
other - the symptom is a pairing that fails on air with no diagnosable cause.

The digest covers the pinned **values** in a canonical form, not the file text,
so rewording a comment cannot raise a false alarm about a set that has not moved
and a reformat cannot hide one that has. The guard that refuses to rewrite a
published set still compares the whole text: the guard exists to make someone
stop and think before touching an artifact, the digest exists to tell consumers
whether anything they depend on moved.

For an hour these digests were **decorative** - three generators computed them,
three headers carried them, and nothing read one, while this paragraph claimed
they made a change visible. Visible only if something looks.

`wire_v1` and `wire_v2` still carry digests computed the old way, over the file
text. They are archived, nothing depends on them, and recomputing them would
mean the numbers in old commit messages could no longer be reproduced without the
old code. Two schemes therefore coexist, deliberately, and this sentence is why
the next reader should not "fix" the inconsistency.

## See also

- [key-lifecycle.md](key-lifecycle.md) — where these keys come from
- [crypto-architecture.md](crypto-architecture.md) — which core runs what
