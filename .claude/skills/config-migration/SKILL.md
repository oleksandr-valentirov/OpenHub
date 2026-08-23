---
name: config-migration
description: Add a runtime parameter to the hub's configuration store, or change the record format, without breaking a board that already holds a store. Use before editing cfg_config_t, cfg_device_t, cfg_snapshot_t or CFG_VERSION, before any change that erases or rewrites flash the store owns, and when a second store has to read the sectors a first one still holds.
---

# Changing the configuration store without breaking a board

The store is `radio_devices_docs/open_hub/arch/config-store.md` and ADR-0027; this
is the **procedure**, and every rule below was paid for by a defect on this bench
rather than reasoned out in advance.

**One sentence first.** Adding a runtime parameter is almost free, and the reason
is a single design property that is easy to destroy by accident. Read §1 before
touching a struct.

---

## 1. Adding a runtime parameter — the cheap path

Put the field in **`cfg_config_t`**, appended, taking from its `spare[]` first.

That is safe on a board that already holds a store, and the reason is not
obvious:

```
cfg_snapshot_t = hdr(16) + cfg(N) + pad(512-16-N) + dev[64]
                 └──────── head is always 512 ────────┘
```

- The roster's offset is fixed by the **head's size**, not by the config's, so
  growing `cfg_config_t` eats `pad` and **moves no device entry**.
- `pad` is written as zero, so on a snapshot already on flash the new field's
  bytes are **zeros**, not somebody else's data.
- No byte moved, so the record's **checksum still validates**. An old snapshot
  stays readable under the new layout, and the new field reads as zero.

**So the whole compatibility question reduces to: is zero a safe value for this
parameter?** If yes, append it and stop — no version bump, no migration, nothing
to write.

Two checks hold that property and both must stay green:

```c
_Static_assert(offsetof(cfg_snapshot_t, dev) == CFG_SNAP_HEAD_BYTES,
               "the roster's offset must not depend on the config's size");
```

and `Common/test/test_cfg.c` asserting the head's padding is zero. The `offsetof`
one is the only check that catches the roster being placed **before** the pad —
a mistake that moves every entry while every size still looks correct.

**What tells you the head is full:** the build says *size of array 'pad' is too
large*. That is the real check; the expression underflows. An assert placed after
the struct to say the same thing cannot ever run, and one was written and removed
for exactly that reason.

### When zero is not a safe default

Then it is not a cheap change. Either:

- give the parameter a **sentinel** whose zero means *not set*, and resolve the
  default at the point of use rather than in the record — usually the right
  answer, and it keeps the cheap path; or
- bump `CFG_VERSION` and go to §2.

**Do not** change what an existing field means while keeping its offset. A field
whose meaning changed under a version that did not is undetectable by every check
in the tree.

---

## 2. Changing the record format — the expensive path

Anything that moves a byte: a new field in `cfg_device_t`, a reordered struct, a
different `CFG_SNAP_HEAD_BYTES`, a different slot size.

1. Bump `CFG_VERSION`. `cfg_record_valid()` refuses a foreign version, so old
   records stop being read rather than being misread — **the difference between
   an empty store and a corrupt one.**
2. Decide what an old snapshot becomes. Usually: read it under the old layout,
   translate, write a new snapshot. That is a migration and it goes through §3.
3. Re-derive the geometry. `CFG_SNAP_SLOTS`, `CFG_CYCLE_SLOTS` and the erase
   budget all follow from the sizes, and the asserts in `cfgstore.h` catch a
   cycle that no longer fits its sector.

---

## 3. The migration procedure

Used once already, end to end, and every step exists because skipping it would
have cost something specific.

**Read → witness → commit, and the witness derives from what flash gives back.**
Deriving it from what you passed in witnesses the caller, not the write.

**Never erase the old copy in the same boot that writes the new one.** Write the
new store into the *other* sector, leave the old readable, and let the **next**
boot reclaim it. At no point is there a moment with no readable copy. The design
page originally said erase-then-write; that order has a window in it and the
boot-erase mechanism removes the need for it.

**Check every accessor you use to read the old store for side effects.**
`ks_net_key_get()` **creates and appends a key** when none is stored — so reading
the old store's network key would have written to the store the step exists to
leave untouched. A migration whose first step mutates its source has no fallback
left.

**Switch the readers before anything erases.** The keys had four call sites in
`pairing.c` and two in `cli.c`. Erasing first would have left the hub holding a
key nothing could read, and the failure would have surfaced at the next pairing
rather than at the erase.

**Prove which store answered.** A fallback that silently covers a broken new path
returns the identical value and looks perfect — until the old store is gone.
`cfg` prints two separate lines for this: the new path with **no fallback behind
it**, and which branch the seam actually took.

**Retire the old reader in the same boot the new store goes live.** Its RAM cache
was built before the new store erased flash underneath it, so it reports records
that no longer exist — and at the next boot it finds an erased sector and appends
into the new store's spare ring. **Two stores sharing two sectors is a state that
must not outlive one boot.** `ks_retire()` is that, and it refuses with its own
reason rather than a shared one.

**A board with no store and no identity needs `cfg gen`**, not a migration. It
draws a scalar and a network key straight into the identity sector. Without it a
virgin board would have to bootstrap through the very store being retired.

---

## 4. Verifying it, in the order that catches things

1. **Host first.** `make -C Common/test check`. The ring's arithmetic is pure and
   every branch of it is mutable — sixteen mutations, sixteen caught. Add the case
   before the code if the change touches scan, replay, wrap or plan.
2. **Mutate the new check.** A test written beside a change agrees with it by
   construction. Two of this store's tests passed for the wrong reason on their
   first run and only a mutation said so.
3. **`cfgflash` on the board.** Eight guards, none of which touches flash. It has
   already refused something nobody arranged for it to refuse.
4. **Read the flash from outside the firmware.** `STM32_Programmer_CLI -r` into a
   file and parse it on the host, recomputing the record's checksum with a
   **separate implementation**. That is the only check that does not share a bug
   with the firmware, and it is what confirmed both the identity record and the
   first snapshot before anything erased.
5. **Reset and confirm the deltas replay.** A store that does not survive a reset
   is not a store.

---

## 5. Traps this store has already fallen into

**`HAL_GetTick()` cannot time an operation that masks interrupts.** The erase
reported **1 ms** instead of ~950: SysTick keeps counting, its interrupt stays
pending, and one tick lands on re-enable. `DWT->CYCCNT` runs regardless of
masking. The measurement that established the erase had used DWT; the driver
written from it did not.

**`--gc-sections` removes a routine that is only ever reached by address.** The
ITCM erase linked at **0 bytes**, both input sections in the map's discarded list,
because nothing in the call graph kept it while the driver had no caller. The
build was clean and the code was correct. `KEEP()` in the linker script, plus
`cfgflash_init()` returning a byte count the erase refuses on.

**A check whose population can be empty must fail on empty.** The overwrite guard
aims at data; on a freshly erased sector it had nothing to refuse and would have
read `ok` while testing nothing. It reports `CFGF_ST_NO_POP` instead, and it
should aim at the live snapshot, which is where data is whenever a store exists.

**Do not map an old store's numeric codes onto a new store's reasons.** The
enrolment renderer was one commit from printing one plausible sentence for every
refusal — which is the defect the store's own failure renderers exist to prevent.
Carry the reason the new call actually returned.

**An erase is legal only below CM7's release of `HSEM_ID_0` and above the
scheduler.** The driver refuses everywhere else, with a distinct reason for each.
That window exists because the release was deliberately moved into `main()`;
`radio_devices_docs/open_hub/arch/dual-core.md` says what may not move above it.

---

## 6. The checklist

- [ ] Field appended to `cfg_config_t`, taking from `spare[]` first
- [ ] Zero is a safe value for it, or there is a sentinel, or `CFG_VERSION` moved
- [ ] `offsetof(cfg_snapshot_t, dev)` assert still green; host suite green
- [ ] A host case for the new behaviour, and a mutation proving it can fail
- [ ] Every reader of the parameter handles the zero an older snapshot gives
- [ ] `cfg` and `cfgflash` read correctly on a board that already holds a store
- [ ] Flash read back from outside the firmware, checksum recomputed separately
- [ ] Reset, and the value survives
- [ ] Bench announced in `bench/RESOURCES.md` before anything erases
