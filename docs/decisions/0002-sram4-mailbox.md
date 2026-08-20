# ADR-0002 — Cross-core IPC through SRAM4 and hardware semaphores

**Status:** Accepted. The message structure is refined by [ADR-0016](0016-sequence-numbered-ipc-ring.md);
the choice of SRAM4 and HSEM stands.
**Date:** 2026-08-19

## Context

[ADR-0001](0001-dual-core-split.md) put the radio and the application on different
cores, so the console has to reach the radio somehow. The H755 offers hardware
semaphores (HSEM) for signalling and several RAM regions reachable from both cores.

D1 RAM is cacheable from CM7, which makes sharing delicate. SRAM4 sits in the D3
domain, is reachable by both cores and by the DMA fabric, and is outside the caches.

## Decision

A struct in SRAM4 carries the payload; HSEM carries the signal.

`Common/inc/hsem_table.h` is the **single allocation table** for semaphore IDs.
`Common/inc/shared_memory.h` defines the request struct and the placement
attribute. Both cores define an object of the same name in `.shared_mem`, and
**both linker scripts place that section at the `RAM_D3` origin**.

The address `0x38000000` therefore appears only in the linker scripts. Neither
core carries it as a literal.

The section is `(NOLOAD)`.

## Consequences

- The two cores agree on the address without either one hardcoding it, and moving
  the region is a one-line change per core.
- `(NOLOAD)` is essential: without it whichever core boots second zeroes the region
  during startup and destroys what the first wrote.
- **The mailbox is a single slot with no sequence number.** A second request
  issued before the first is answered overwrites it. Acceptable today because only
  the CLI produces requests and a human is the rate limiter.
- **This blocks other work.** Automatic producers — a scheduler pushing session
  keys after pairing or rotation ([ADR-0011](0011-mbedtls-on-cm7-only.md)) — will
  race the CLI. A sequence-numbered ring is required *before* that code is written,
  not after.
- No reply timeout: a CM4 that stops answering leaves CM7 waiting.
- No struct versioning. Both images come from one tree, so a mismatch means someone
  flashed halves of two builds — tolerable now, not once updates are done in the
  field.

## Alternatives rejected

**OpenAMP / rpmsg.** Present in the Cube package and the "proper" answer. Rejected
as far too heavy for what is currently a handful of register reads, and it would
put a message framework on the core deliberately kept free of frameworks.

**A raw address constant in both sources.** Two literals that must agree, with
nothing checking them. The linker already solves this.

**Sharing through D1 RAM.** Cacheable from CM7; every exchange would need cache
maintenance, and getting that subtly wrong yields intermittent corruption.

## See also

[architecture/ipc.md](../architecture/ipc.md)
