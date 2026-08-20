# ADR-0015 — All RNG access goes through a guarded service

**Status:** Accepted
**Date:** 2026-08-20

## Context

Every key in the system begins as RNG output, so a silent RNG failure would
compromise everything downstream while looking perfectly normal.

Three facts, each verified rather than assumed:

- **The HAL cannot report a seed error on this part.** The check inside
  `HAL_RNG_GenerateRandomNumber` is wrapped in `#if defined(RNG_CR_CONDRST)`, and
  the STM32H755 RNG has no `CONDRST`. `HAL_RNG_Init` never touches `RNG_SR`
  either. ST's own note says a word drawn under a seed error "must not be used
  because it may not have enough entropy" — and nothing on this chip would say so.
- **`SEIS` latches spontaneously.** Cleared by hand over SWD, it returns within
  about a second of idling with nothing reading the RNG. `SECS` stays clear
  throughout, so the generator itself is healthy; the flag simply latches and
  stays.
- **The one existing consumer did not hold its lock.** The ping id draw tested
  `HAL_HSEM_IsSemTaken` instead of holding the semaphore, ignored the result of
  `FastTake`, and used the word regardless of any error flag.

## Decision

All RNG access goes through `Common/src/rng.c`. No caller touches `RNG->DR`.

Per draw: drop the buffered words, clear `SEIS`/`CEIS`, wait for `DRDY` on a
bounded spin, read `DR`, then test `SEIS`/`SECS` and reject the word if either is
set. Retry through a generator restart. Everything under `HSEM_RNG`.

`rng_init()` is called from **CM7**, which is the core whose generated `main()`
actually calls `MX_RNG_Init()`.

## Consequences

- **Only words vouched for by a flag check reach a caller.** Clearing before and
  testing after is what makes the check mean anything: `SEIS` is cleared
  immediately before the draw so it can only latch from the word being drawn.
- Buffered words are dropped, because `DRDY` stays high for four words and those
  predate the clear — nothing can vouch for them.
- **A per-draw check is the only workable design**, given the idle latching. A
  startup check would pass and say nothing about later draws; an idle check would
  fail constantly on a healthy generator.
- Failures propagate as status codes. Callers that do not need cryptographic
  quality may fall back; key derivation must not, and must not be written as if
  the call cannot fail.
- Cost is a handful of register accesses per word — irrelevant at the rate keys
  are drawn.
- The `rng` console command makes the state observable without a debugger.

## Alternatives rejected

**Trust `HAL_RNG_GenerateRandomNumber`.** It cannot report the failure on this
part. This is the status quo that had to be replaced.

**Clear `SEIS` once at startup.** The first implementation. It does not survive
the idle latching, and worse, it *appears* to work — the flag reads clear right
after startup.

**Refuse the words drawn during a restart.** Also tried. Those are exactly the
words that trip the entropy checks while conditioning settles, so refusing them
left `SEIS` permanently latched and every draw failing.

**Check `SECS` only, ignoring `SEIS`.** `SECS` is the current status and clears
itself, so a transient error window between two reads would go unnoticed —
which is precisely the case that produces a low-entropy word.

## See also

[security/entropy.md](../security/entropy.md),
[ADR-0013](0013-crypto-peripheral-ownership.md)
