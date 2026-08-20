# ADR-0016 — Sequence-numbered rings replace the single-slot mailbox

**Status:** Accepted
**Date:** 2026-08-20

Refines [ADR-0002](0002-sram4-mailbox.md), which keeps its choice of SRAM4 and
HSEM. Only the message structure changes.

## Context

The mailbox was one struct with no sequence number. [ADR-0002](0002-sram4-mailbox.md)
recorded this as tolerable because only the CLI produced requests, and named it as
a hard prerequisite for anything automatic — the session-key install path in
[ADR-0011](0011-mbedtls-on-cm7-only.md) in particular.

The sharper problem was not overwriting but **reply misattribution**. CM7 timed
out after 500 ms and moved on; a late reply then sat in the slot and was consumed
as the answer to the *next* request. With one slot and no identifier there is no
way to tell the two apart, and the failure is silent — a plausible wrong value.

## Decision

Two single-producer/single-consumer rings in SRAM4, eight slots each, plus a
header carrying a magic number and a protocol version.

Every request carries a sequence number; every reply echoes it. A reply whose
sequence number nobody is waiting for is **discarded and counted**.

The MPU maps SRAM4 strongly-ordered on CM7 (TEX 0, C 0, B 0, S 1), so `head` and
`tail` need a `__DMB()` between the slot write and the index update, but **no
cache maintenance and no semaphore on the data**. One writer per index is what
makes that safe.

HSEM becomes a **doorbell, not a lock**. CM4 drains by polling the ring, so a lost
pulse costs latency rather than a message.

CM7's requesters are serialised by a FreeRTOS mutex: the ring tolerates several in
flight, but a caller waiting on its own sequence number must not have another
thread drain the reply out from under it.

## Consequences

- **A late reply can no longer be mistaken for the current one.** This is the
  actual fix; the ring capacity is secondary.
- The magic and version stamp means a core built from a different tree is
  detected rather than silently misinterpreting the layout. `ipc_ready()` gates
  both sides.
- `ipc_init()` runs on CM7 **before** CM4 is released from `HSEM_ID_0`, so CM4
  never reads a ring holding whatever `(NOLOAD)` left in SRAM4.
- CM4 answers **every** request, including ones it does not handle
  (`IPC_ST_UNKNOWN_REQ`). A caller waiting on a sequence number must never be left
  waiting because a request type was unhandled — the previous code simply fell
  through.
- The console distinguishes "did not answer" from "answered with an error". They
  need different debugging and were previously reported identically.
- 920 bytes of SRAM4, against 64 KB available.
- Observable: `ipc` prints the header, both ring depths, and the stale-reply
  count. A non-zero stale count means requests are timing out and CM4 is
  answering late — the exact condition that used to corrupt the next exchange.

## Verified

`device dump 10` returns `0x24` (RegVersion). An unhandled `device remove` reports
`status 1` and the next request is still correct — the old design would have
desynchronised. Both rings drain to `head == tail`; stale count stays zero. Both
cores place `shared_ipc` at `0x38000000` with an identical 0x398-byte section.

## Alternatives rejected

**Keep one slot, add a sequence number.** Fixes misattribution but not
overwriting, and gives CM4 no room to answer a burst.

**A mutex or semaphore around the shared struct.** Unnecessary: one writer per
index on strongly-ordered memory is already safe, and a lock across two cores with
different scheduling models adds a way to deadlock.

**Let CM7 issue several requests concurrently.** The ring supports it, but then
two waiters race to drain each other's replies. Serialising CM7 is cheap and the
request rate is a human at a console.

## See also

[architecture/ipc.md](../architecture/ipc.md)
