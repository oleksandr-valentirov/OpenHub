# The flash store

**Status: the durable superframe counter is implemented and verified. Key
material is not — that store is next and lives on CM7.**

## Why the counter needs flash at all

The superframe counter is the most significant field of the
[GCM nonce](../security/wire-crypto.md#nonce-construction). A hub that restarts
it at zero and reuses a persisted session key repeats nonces, and a repeated
nonce does not merely leak plaintext — it leaks the authentication subkey and
lets an attacker forge frames.

Until this existed, the rule was that **a hub reboot must invalidate sessions and
force re-pairing**. That was a real constraint, not a formality.

## A ceiling is stored, not the counter

Writing the counter every 2 s would wear the flash out and put a bus stall in the
middle of the slot grid. Instead the hub writes a value `KV_RESERVE_AHEAD = 4096`
superframes in the *future*, and the next boot starts there.

```
boot 1:  counter 10     ->  reserved 4096
boot 2:  counter 4106   ->  reserved 8192
boot 3:  counter 8202   ->  reserved 12288
```

Nothing at or below a stored ceiling is ever reused, at one write per 2.3 hours.

The cost of an unclean shutdown is **skipping** up to 4096 counter values. That
is free: the space is 2^32 — about 272 years — and skipping is the conservative
direction. It is the same argument [key-lifecycle.md](../security/key-lifecycle.md#the-replay-floor-belongs-to-a-key-not-to-a-device)
makes for the transmit floor.

**A device sees a hub reboot as a forward jump of up to 4096.** Forward is the
direction a replay check accepts, but it is large enough that a device's
plausibility check may refuse it; the device side recovers by re-taking a beacon
on trust after eight refusals, about 16 s.

## The erase cannot happen in service

The constraint that shaped everything else.

H7's minimum erase is a whole **128 KB sector**, it stalls the bank the core is
executing from, and it can take up to **1.4 s**. CM4's watchdog is IWDG2 at
prescaler 4 / reload 4095 = **512 ms**, and the LSI driving it is specified only
to ±50%, so the real budget can be as short as ~348 ms. An in-service erase
would reset the radio core.

So the spare sector is **erased at boot**, where a stall is harmless, and the log
switches to a sector that is already erased. An in-service erase becomes an
in-service *write*.

If both sectors fill without a reboot — 8192 records at one per 2.3 h, about 2.1
years of continuous uptime — the store **refuses to write**, and the radio
**stops transmitting**.

That second half matters more than the first, and the code did not have it at
first. Refusing to write leaves the counter advancing past the ceiling anyway, so
a future boot hands those values out again — which is precisely the nonce reuse
the store exists to prevent. The comment claimed refusing was "the safe
direction" while nothing acted on the refusal, making the whole store's purpose
conditional on a path that had never run.

`kv_counter_safe()` now gates the beacon. The counter still advances — it is the
protocol's clock, and stalling it repeats superframe numbers, which is the same
defect by the other route — but nothing is transmitted. A reboot compacts and
recovers.

The refusal is also **latched**. `kv_reserve()` is called on every superloop pass,
so a write that cannot succeed was being retried thousands of times a second:
measured at **1.79 million attempts in 45 s** during the test below. A flash that
refuses one write will refuse the next.

## The record

32 bytes, because that is the H7's minimum programmable unit and a flash word can
be written exactly **once** between erases. A record is written whole and never
revised.

```c
typedef struct kv_record {
    uint32_t magic;         /* 'OHKV' */
    uint8_t  version;
    uint8_t  type;
    uint16_t pad;
    uint32_t seq;           /* highest valid wins; survives the sector swap */
    uint32_t counter_mark;  /* nothing at or below this may be reused */
    uint32_t key_gen;       /* the generation the floor below belongs to */
    uint32_t rx_floor;      /* replay floor, scoped to key_gen, never to a device */
    uint32_t spare;
    uint32_t crc;
} kv_record_t;
```

`key_gen` and `rx_floor` are carried although pairing does not exist yet, for two
reasons. The format has to stop changing before anything depends on it. And a
floor that **names its generation is detectably stale** rather than merely cleared
at the right moment — clearing by call site works until someone moves the call.
That distinction came from the device side, where a stored floor of 5000 once
correctly refused a legitimate frame at 1001: the mechanism was right and the
model was wrong.

`seq` is a monotonic record index rather than a position, so the newest record is
identifiable across a sector swap without knowing which sector is active. A torn
write fails its CRC and is skipped rather than stopping the scan.

## Layout

| | |
|---|---|
| Sectors | 6 and 7 of bank 2 — `0x081C0000`, `0x081E0000` |
| Slots | 8192 |
| Write rate | one per 4096 superframes, ~2.3 h |

CM4's code is 42 KB in sector 0 of the same bank and is not going to grow into
these.

## Writes must not touch the beacon

`kv_reserve()` runs **only in the first half of the superframe**. A flash program
stalls the core, and the beacon's offset within the superframe — 1–7 µs — is the
number every device's period estimate rests on
([tdma.md](../radio/tdma.md#the-beacon-must-leave-at-a-fixed-offset-and-does)).
There are 4096 superframes of opportunity, so a write never needs to happen near
a boundary.

Measured rather than assumed. A write happens once per 2.3 h in production, so
the path was stress-tested at `KV_RESERVE_AHEAD=2` — **2048× the production
rate** — across 31 writes:

| | beacon late |
|---|---|
| production | min 3, max 7 µs (spread 4) |
| 2048× write rate | min 3, max 9 µs (spread 6) |

A flash program costs about 3 µs of worst case, at a rate three orders of
magnitude above production. 6 µs against a 2 s superframe is **3 ppm**, far below
the ±1% a device clamps its period estimate to.

## The append point is the first erased slot

Not one past the last *valid* record. Those differ whenever a slot holds
something the scanner rejects — an unreadable record, or one of an older format —
and deriving the append point from valid records alone aims the next write at
occupied flash.

**On this store that consequence is the worst in the project.** An H7 flash word
cannot be programmed twice, so the write fails, `exhausted` latches,
`kv_counter_safe()` goes false and the radio stops transmitting — correctly,
since an unreserved counter is nonce reuse. But **a reboot does not clear it**:
the boot erase cleans the *spare*, the active sector keeps the bad record, and
the same append point is computed again. One unreadable record would have
silenced the hub permanently.

It was found on the device side, whose store has the same shape and where the
same bug instead fell through to a **page erase on every write** — silent,
reported as success, and reaching the most dangerous operation in that system for
no reason. Two stores, one bug, two completely different symptoms, and neither
side found it by reading the function.

Verified by injecting a record with a deliberately wrong CRC and a `seq` **higher
than any valid record**, so a scanner that accepted it would win the comparison
and adopt its counter mark:

| | |
|---|---|
| mark after reset | 94283, the previous ceiling — not the injected `0xDEADBEEF` |
| the bad record's slot | occupied, not reused |
| the hub afterwards | 12 beacons, 0 silent, 1 write, 0 errors |

A record here is exactly one flash word and the H7 programs 256 bits or fails,
so a *torn* record in the multi-word sense is not reachable on this store —
unlike CM7's, which is four words. What is reachable is a slot the scanner
cannot accept, and corruption and an unknown format present identically.

## The failure paths are exercised, not reasoned about

Every path below only runs when something is already wrong, which is the category
that has produced three bugs between the two sides of this project so far. They
were forced rather than argued.

**Record size.** `_Static_assert(sizeof(kv_record_t) == KV_RECORD_BYTES)` — and
the assert was checked by adding a field and confirming the build stops. Note it
asserts the record *fills* a flash word, not that it fits in one: a short record
leaves filler a future field could quietly claim, and a long one is two flash
programs where the code performs one.

**Exhausted log.** Rebuilt with `KV_RESERVE_AHEAD=0` so the ceiling can never get
ahead of the counter:

```
counter 24662, reserved to 24651 (-11 ahead)
flash: 8149 writes, 1794330 errors, 0 slots left
STOPPED: 12 superframes sent nothing to avoid nonce reuse - reboot to compact
beacons 0 (0 announce), 0 silent
```

**Zero beacons transmitted**, the counter still advancing, and the condition
reported. The 1.79M errors are what produced the latch above.

**Boot-time recovery from a full log.** The test left the store with both sectors
full — which is the state the boot erase exists for and had never been in. On the
next boot:

```
counter 24662, reserved to 28747 (4085 ahead)
flash: 1 writes, 0 errors, 4095 slots left
beacons 12 (0 announce), 0 silent
```

The spare was erased, the ceiling resumed above the last reserved value, and the
radio came back. Full recovery from an exhausted store, by reboot alone.

## A latent bug this surfaced

`calib_init()` waits up to 500 ms for the LSE against a 512 ms watchdog whose LSI
is ±50% — so the real margin could be negative. The comment above that wait says
a dead crystal must not hang the radio core, and with the watchdog running, a
dead crystal would have turned graceful degradation into a **boot loop**.

It never fired because the wait exits in ~10 ms when the crystal is alive: the
only path that could trigger it was the defensive one. Fixed by feeding the
watchdog inside the wait. Worth recording because the shape recurs — a
protective path that has never run is a path that does not work.

# The device store on CM7

**Status: enrolment implemented and verified. The key exchange that fills in the
root key does not exist yet.**

Same shape as the counter store above — log-structured, newest record per key
wins — in **bank 1, sectors 6 and 7** (`0x080C0000`, `0x080E0000`). CM7's code
is 179 KB in sectors 0–1.

**But this store never erases anything, and that is not a preference.**

## Erasing bank 1 from CM7 hangs the core and bricks the board

The most expensive thing found in this project so far, and it cost the board
twice before it was understood.

CM7 fetches instructions from bank 1. A 128 KB sector erase in bank 1 **never
returns**: a single erase issued from an ordinary FreeRTOS task killed the
console, and SWD memory reads of the store sectors stalled with it.

The second-order effect is what makes it serious. The interrupted erase leaves
the sector with **uncorrectable ECC**, and reading such a flash word raises a bus
fault. `scan()` reads those sectors at every boot, before the scheduler starts —
so the board then hard-faulted on **every** subsequent power-up, with a perfectly
good firmware image. Reflashing did not help, because the fault is in data the
firmware reads rather than in the firmware. Recovery needed an external
programmer:

```bash
STM32_Programmer_CLI -c port=SWD sn=<probe> mode=UR -e 6 7
```

**CM4's equivalent in bank 2 does work** and has been observed recovering a full
log. Same code, different bank: CM4 does not fetch from bank 2 during its own
erase in the way CM7 does from bank 1. So this is a property of erasing the bank
you execute from, not of the design.

Consequences, accepted rather than worked around:

- **The spare is checked and never erased.** `ks_init()` reports whether it is
  clean and nothing acts on a dirty one.
- **Records of an older format are stepped over, not reclaimed.** The append
  point is the first *erased* slot, so stale records cost a slot each and
  nothing more.
- **There is no `erase_sector()` in this file.** An unused one would be a loaded
  gun for the next person to call.
- **A full store is not recoverable by reboot**, unlike CM4's. It holds 2048
  records and reclaims none. For 64 devices at a few records per device lifetime
  that is not a capacity anyone reaches; if it ever matters, the fix is an erase
  routine resident in ITCM with interrupts masked, not a boot-time erase running
  from flash.

`ks_init()` still runs **before `osKernelStart()`**, because `scan()` walks
256 KB and should not do it under the scheduler.

## The record

128 bytes — four flash words exactly, asserted, and the assert checked by adding
a field and confirming the build stops. It caught a miscount of my own the moment
`rotate_epoch` was added, which is the entire argument for having it.

```c
typedef struct ks_record {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  state;        /* ENROLLED | PAIRED | DELETED */
    uint8_t  slot;         /* uplink slot, assigned at enrolment */
    uint32_t seq;          /* newest record for a dev_id wins */
    uint32_t dev_id;
    uint32_t key_gen;      /* the generation rx_floor belongs to */
    uint32_t rotate_epoch; /* the epoch this root key was established at */
    uint32_t rx_floor;     /* scoped to key_gen, never to a device */
    uint32_t tx_floor;     /* carried forward across pairings */
    uint8_t  fingerprint[32];  /* SHA-256 of the device public key */
    uint8_t  root_key[32];     /* HKDF root; zero until pairing lands */
    uint8_t  spare[28];        /* named, so the next field needs no migration */
    uint32_t crc;
} ks_record_t;
```

### Rotation needs an epoch, and no wall clock

The daily ratchet is indexed by the superframe counter — `superframe /
SUPERFRAME_PER_DAY`, 43200 at a 2 s superframe — and **not** by wall time. There
is no RTC on either core, and a clock that reset to 1970 on a power cut would
silently re-derive keys that had already been used, which is the same nonce-reuse
shape [the durable counter](#why-the-counter-needs-flash-at-all) exists to
prevent. The counter survives a reboot; a wall clock would not.

`rotate_epoch` is the epoch the root key was agreed at. The current ratchet key
is `ratchet(root_key, now_epoch - rotate_epoch)`, so it is always derivable from
what is stored plus the shared counter — **rotation costs no flash writes at
all**, only HKDF steps. It has to be stored because the ratchet is not stateless:
`K(n)` cannot be computed without knowing which `n` the root corresponds to.

**The store reports `key_gen` and does not judge staleness.** Only the holder of
the current session key knows which generation is in use, so a staleness flag
computed here would always read false — and a flag that is always false is worse
than no flag, because it looks like a check. The comparison belongs where the
answer is knowable. `key_gen` advances on **every** clear, whether or not a floor
was stored, so a floor written later can never be read as belonging to an earlier
key. Both rules come from the device side.

## Enrolment rules

- **The lowest free slot is assigned**, counting from zero. That is not
  arbitrary: the [join region](../radio/pairing.md) overlays the tail of the
  uplink region, so unassigned slots are the ones it displaces.
- **Re-enrolling keeps the slot and the transmit floor and clears the receive
  floor**, advancing `key_gen`. A receive floor left over from a previous pairing
  locks out the very device that just paired, silently and permanently.
- **Removal is a tombstone**, since the log is append-only. The slot is freed and
  the transmit floor is kept — carrying it forward only skips counter space,
  which is free.
- **Device id 0 is refused.** It is what an uninitialised variable holds, and an
  id that can be produced by forgetting to set one will eventually be set by
  forgetting.
- **A fingerprint must be exactly 64 hex digits.** One that parsed short would
  authenticate against a prefix, which is precisely what it exists to prevent.
- **Enrolment persists before the pairing window opens.** A window opened for a
  device whose fingerprint did not reach flash is pairing without
  authentication, and looks identical to pairing with it.

## What is stored in the clear

`root_key` will hold long-term secret material in plain flash. There is no
secure element on this board and no readout protection configured. The
alternative — not persisting it — means re-pairing all 64 devices after a power
blip, which is worse operationally and no better once an attacker has the board
in hand. Named here rather than left implicit.

## Verified

```
device add 5a3c91e7 3a7f91c2...            -> enrolled in slot 0, window open
device add b82d4f16 <60 hex digits>        -> rejected: must be exactly 64
device add 0 <valid>                       -> rejected: id 0 is not usable
device remove 5a3c91e7                     -> removed
device add c1d2e3f4 9988...                -> enrolled in slot 0  (freed slot reused)
```

Enrolment survived a reset, and then survived **reflashing both cores** — the
store is in sectors the ELF does not touch.

**Torn records**, using the trick the device side proposed: write a record short
by its final flash word — byte-identical on flash to a write torn at that point —
and give it a **higher `seq` than any valid record**, so a scanner that accepted
it would win the comparison. A torn record with a low `seq` would be ignored for
the wrong reason and prove nothing.

| | |
|---|---|
| ignored despite the winning `seq` | both real enrolments intact, the tombstone id absent |
| occupies its slot | 2046 → 2045 free, and still 2045 after a reset |
| the store works past it | next enrolment landed in slot 2 |

The third is the property worth having and the one not obvious to assert: "the
scanner skips bad records" and "the store keeps working after one" are different
claims, and only the first follows from the CRC.

**What this does not cover:** it tests that a partial record is indistinguishable
from an absent one, which is what the recovery argument rests on. It does not
test that the supply can only tear a write in this shape — if flash can leave a
word half-programmed, bits set but not all of them, that is a different input and
this does not reach it.

## The hub's own identity

`device hubkey gen` creates the hub's long-term P-256 keypair once and stores the
**private half only**, in a `KS_TYPE_HUBKEY` record. `device hubkey` prints the
compressed public key, in full, because that is what gets provisioned into
devices and a truncated value transferred out of band would weaken the binding to
its truncation.

**Regeneration is refused.** Replacing this key orphans every device ever paired:
each holds `hub_static` from provisioning, and a hub that cannot prove the
matching private key fails `Z1` and therefore every pairing it already has. There
is no "regenerate" that is not a fleet re-provision, so the command says that
rather than offering a `--force`.

Only 32 bytes are stored. The public half is recovered with one scalar
multiplication when asked — about 167 ms, on a path that runs at most once per
boot — which avoids overloading record fields to squeeze a 33-byte point into a
layout that has no room for one. `crypto_p256_public()` runs
`mbedtls_ecp_check_privkey()` first, so a corrupted or **all-zero** stored key
fails there rather than yielding a usable-looking point; an all-zero private key
is exactly what a missed keygen failure leaves behind.

The record's `dev_id` is zero, which cannot collide with any device because
`ks_enrol` refuses zero — a rule adopted for a different reason that turned out
to make this safe by construction.

Verified: generated, printed, refused on a second attempt, and survives a reset
alongside four enrolled devices.

## Still to build
- **The exchange itself**, which is what turns a stored fingerprint into
  authentication. See [key-lifecycle.md](../security/key-lifecycle.md#provisioning).

## See also

- [radio/tdma.md](../radio/tdma.md) — the counter this protects
- [security/wire-crypto.md](../security/wire-crypto.md#nonce-construction)
- [security/key-lifecycle.md](../security/key-lifecycle.md)
