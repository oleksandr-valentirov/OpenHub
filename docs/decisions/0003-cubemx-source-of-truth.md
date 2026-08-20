# ADR-0003 — The `.ioc` is the source of truth

**Status:** Accepted
**Date:** 2026-08-19

## Context

CubeMX regenerates `main.c`, `freertos.c`, `ethernetif.c`, `lwip.c` and both
`mx-generated.cmake` files, preserving only `USER CODE` blocks. It is tempting to
hand-edit generated files for a quick change — a clock, a pin, an init parameter.

## Decision

**Anything CubeMX can generate must be generated.**

Before hand-editing a file, check whether the `.ioc` owns it. If it does, change
the `.ioc` and regenerate. Hand-written code goes in `USER CODE BEGIN/END`, in
files CubeMX never touches, or in the two `CMakeLists.txt` files that are generated
once and then owned by the project.

Regeneration is scripted and headless; the recipe lives in the `cubemx` skill.

## Consequences

- The GUI always shows the truth. This is the real payoff — the failure mode of
  hand edits is not "the edit is lost", it is "the next person makes decisions from
  a picture that does not match the firmware".
- Every configuration change costs a 2–3 minute generate cycle.
- A **control run** is required before any `.ioc` change: generate from the
  committed file and confirm `git status` is clean. Otherwise the diff after the
  real change is unreadable.
- A list of template gaps must be re-applied after each generation — stack sizes in
  bytes, missing FreeRTOS includes, shared EXTI vectors, `TxConfig`. These are
  documented in [architecture/build-and-generation.md](../architecture/build-and-generation.md);
  every one has cost real debugging time.
- **Adding a peripheral is unreliable from a script.** Removal is automatic;
  addition needs seven interlocking keys, and the position of the
  `VP_<inst>_VS_<sig>` entry within `Mcu.Pin<n>` is load-bearing — an entry appended
  out of order is silently deleted with nothing in the log. Practical rule: **GUI to
  add a peripheral, scripts to change one that exists.**
- One deliberate exception exists: the mbedTLS version CubeMX can offer is out of
  support, so that library is vendored instead. See
  [ADR-0011](0011-mbedtls-on-cm7-only.md).

## Alternatives rejected

**Generate once, then own the code.** Common practice, and it ends with an `.ioc`
that describes a firmware that no longer exists.

**Hand-write everything.** Rejected outright — the clock tree alone is not worth
maintaining by hand on a dual-core H7.

## See also

[architecture/build-and-generation.md](../architecture/build-and-generation.md),
`.claude/skills/cubemx/SKILL.md`
