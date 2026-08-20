# Cross-core IPC

**Status: implemented, with known limits — see the bottom of this page.**

Two mechanisms, used together: hardware semaphores for signalling, a struct in
SRAM4 for the payload.

## Hardware semaphores

`Common/inc/hsem_table.h` is the single allocation table. Nothing may take a
semaphore that is not listed there.

| ID | Name | Purpose |
|---|---|---|
| 0 | `HSEM_ID_0` | boot handshake — CM7 holds it, CM4 sleeps on it |
| 1 | `HSEM_RNG` | guards the RNG, which either core may reach |
| 2 | `HSEM_M7_TO_M4_RFM` | CM7 has filled the mailbox with a request |
| 3 | `HSEM_M4_TO_M7` | CM4 has answered |

`HSEM_RNG` exists because RNG sits in the D2 domain and is addressable from both
cores. Two cores reading `RNG_DR` concurrently can each get half a conditioned
word. It is taken by [the guarded RNG service](../security/entropy.md), which is
the only thing permitted to touch `RNG->DR`. Peripheral ownership in general is in
[ADR-0013](../decisions/0013-crypto-peripheral-ownership.md).

## The shared mailbox

`Common/inc/shared_memory.h` defines the attribute that places it:

```c
#define SHARED_MEM __attribute__((section(".shared_mem"), used, aligned(4)))
```

Both cores define `shared_ipc` in `.shared_mem`, and **both linker scripts place
that section at the `RAM_D3` origin** — SRAM4, `0x38000000`. So the address is the
linker's business. Neither core carries `0x38000000` as a literal, and a change to
the region is made in one place per core rather than chased through source files.

Verified: both ELFs put `shared_ipc` at `0x38000000` with an identical 0x398-byte
section.

SRAM4 is the right region because it lives in the D3 domain, is reachable by both
cores and by the DMA fabric, and is outside the caches that make D1 sharing
delicate. The MPU maps it **strongly-ordered** on CM7 — TEX 0, C 0, B 0, S 1 — so
accesses complete in program order with no caching and no buffering.

`(NOLOAD)` on the section matters: without it, whichever core boots second would
zero the area during startup and destroy what the first core had already written.

## Structure

```c
typedef struct ipc_shared {
    volatile uint32_t magic;      /* 'OHB1' */
    volatile uint32_t version;
    ipc_ring_t req;               /* CM7 -> CM4 */
    ipc_ring_t rsp;               /* CM4 -> CM7 */
} ipc_shared_t;                   /* 920 bytes of the 64 KB available */
```

Each ring is eight slots with a `head` the producer owns and a `tail` the consumer
owns. One writer per index on strongly-ordered memory means the rings need a
`__DMB()` between the slot write and the index update — and **no cache maintenance
and no semaphore on the data**.

Every message carries a **sequence number**; every reply echoes its request's.

## Flow

1. CM7 pushes a request and pulses `HSEM_M7_TO_M4_RFM`.
2. CM4 drains the request ring in its superloop, serves each message, and pushes a
   reply carrying the same sequence number, pulsing `HSEM_M4_TO_M7`.
3. CM7 polls the reply ring for **its own** sequence number.

**HSEM here is a doorbell, not a lock.** CM4 drains by polling, so a lost pulse
costs latency rather than a message. The data is already safe without it.

CM7's requesters are serialised by a FreeRTOS mutex. The ring tolerates several
messages in flight, but a caller waiting on a sequence number must not have
another thread drain the reply out from under it.

`ipc_init()` runs on CM7 **before** CM4 is released from `HSEM_ID_0`, so CM4 never
reads a ring holding whatever `(NOLOAD)` left behind.

## Why the sequence number

CM7 gives up after 500 ms. Under the previous single-slot mailbox a late reply
then sat in the slot and was consumed as the answer to the **next** request — and
with nothing to identify it, silently. The caller got a plausible wrong value.

The sequence number makes a late reply detectable, so it is dropped and counted
instead. `ipc` prints that count; non-zero means requests are timing out and CM4
is answering late.

CM4 also answers **every** request, including types it does not handle
(`IPC_ST_UNKNOWN_REQ`). A waiter must never block because a request type fell
through a switch.

Reasoning in [ADR-0016](../decisions/0016-sequence-numbered-ipc-ring.md).

## Remaining limits

- **No retransmission.** A request lost because the ring was full is reported to
  the caller, not retried.
- **The version is checked, not negotiated.** A mismatch disables the mailbox
  rather than falling back, which is right while both images ship from one tree.

## See also

- [dual-core.md](dual-core.md) — the boot handshake that uses `HSEM_ID_0`
- [memory-map.md](memory-map.md) — where `.shared_mem` sits
- [security/crypto-architecture.md](../security/crypto-architecture.md) — the
  session-key install path that will need the ring
