# Entropy and the hardware RNG

**Status: implemented and verified on hardware.**

Every key in the system starts as RNG output
([key-lifecycle.md](key-lifecycle.md)), so this is the one component where a
silent failure costs everything downstream. It had two defects, both silent.

## What was wrong

**The HAL never checks for a seed error on this part.** `HAL_RNG_GenerateRandomNumber`
does check — but the entire check sits inside `#if defined(RNG_CR_CONDRST)`, and
the STM32H755's RNG has no `CONDRST`. `HAL_RNG_Init` does not touch `RNG_SR`
either. So a seed error is invisible to every HAL caller on this chip, and ST's
own note is explicit about the consequence:

> If a number is available in the RNG_DR register, it must not be used because it
> may not have enough entropy.

**The one existing consumer did not hold its lock.** The ping id draw in
`networking.c` read:

```c
while (HAL_HSEM_IsSemTaken(HSEM_RNG) && !((RNG->SR & RNG_SR_DRDY) != 0U)) {}
HAL_HSEM_FastTake(HSEM_RNG);
```

It *tested* the semaphore instead of holding it, ignored the result of
`FastTake`, and used the word regardless of any error flag.

## The measured behaviour

`RNG_SR` reads `0x41` — `DRDY` plus `SEIS` — with `SECS` clear. So a seed error
occurred, the generator recovered from it on its own, and the latched flag stayed
behind with nothing to clear it.

The important finding came from clearing `SEIS` by hand over SWD and watching it:

| Observation | Result |
|---|---|
| Write 0 to `RNG_SR` | `0x41` → `0x01`, the flag does clear |
| Poll again ~1 s later, nothing reading the RNG | back to `0x41` |
| 32 consecutive draws through the guarded path | all succeed, `RNG_SR` = `0x01` after |

**`SEIS` latches on its own within about a second of idling.** That makes it
useless as a persistent health indicator — a check at startup would pass and tell
you nothing about the next draw, and a check "is SEIS clear?" at any idle moment
fails while the generator is perfectly healthy.

## The protocol

`Common/src/rng.c` wraps every access. Per draw:

1. **Drop whatever is already buffered.** `DRDY` stays high until four words are
   read out, and those words were produced before the flag was cleared, so
   nothing below can vouch for them.
2. **Clear `SEIS`/`CEIS`.**
3. Wait for `DRDY`, watching `CECS` for a clock fault, on a bounded spin.
4. **Read `RNG_DR`.**
5. **Test `SEIS` and `SECS`.** If either is set, the word was generated inside an
   error window — discard it, restart the generator, retry.

The order is the whole point: the flag is cleared immediately *before* the draw
and tested immediately *after* it, so it speaks only for the word actually being
handed out. Checking after the read also matters because once `DR` is read the
word is gone and the caller has no way left to judge it.

Restart is ST's documented recovery for an RNG without `CONDRST`: clear the flag,
toggle `RNGEN`, then draw and drop eight words while the conditioning settles.
Those words are dropped **without being judged** — the first attempt at this
refused them instead, which is how it left `SEIS` latched forever.

Every access is taken under `HSEM_RNG`, on a bounded spin, because the peripheral
sits in D2 and both cores can reach it.

## API

```c
rng_status_t rng_init(void);                  /* once, after MX_RNG_Init */
rng_status_t rng_word(uint32_t *out);
rng_status_t rng_bytes(void *dst, size_t len);
uint32_t     rng_last_status_reg(void);       /* raw SR from a failing draw */
```

Failures are reported, never papered over: `RNG_ERR_SEED`, `RNG_ERR_CLOCK`,
`RNG_ERR_TIMEOUT`. Callers that genuinely do not need cryptographic quality — the
ping id — may fall back; callers deriving keys must not.

## Which core initialises it

**CM7.** The `.ioc` lists RNG under `CortexM4.IPs`, but
`ProjectManager.functionlistsort` emits the `MX_RNG_Init()` *call* on CM7 and
leaves CM4 with a defined-but-never-called copy. The generated code is what runs,
so `rng_init()` sits in CM7's `RNG_Init 2` user block.

This also matches boot order: CM4 sleeps until CM7 releases `HSEM_ID_0`, so CM7
necessarily initialises the RNG before CM4 could use it
([architecture/dual-core.md](../architecture/dual-core.md)).

The IPs/functionlistsort disagreement is a real `.ioc` inconsistency and is worth
tidying, but the generated call placement is not ambiguous.

## Checking it

```
rng          draw 4 words and report
rng 32       draw 32
```

The command prints `RNG_SR` on entry — usually `0x41`, flagging the idle latch —
then each word, then `RNG_SR` after. Healthy output ends `0x00000001 ... healthy`.

Verified: 32/32 draws succeed, all words distinct, bit balance 491/1024 ones
(~1.3σ from even). That is a smoke test that the generator is running, **not**
entropy validation.

## See also

- [ADR-0015](../decisions/0015-guarded-rng-access.md)
- [crypto-architecture.md](crypto-architecture.md)
- [key-lifecycle.md](key-lifecycle.md)
