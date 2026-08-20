# ADR-0014 — Console on the ST-Link VCP, interrupt-driven, no stdio

**Status:** Accepted
**Date:** 2026-08-19

## Context

The hub had its console on UART4 (PC10/PC11), which needs an external USB-serial
adapter and its own wiring. The board already exposes USART3 (PD8/PD9) through the
ST-Link as a virtual COM port over the same USB cable used for flashing.

The original CLI also read input by polling with `getchar`.

## Decision

The console is on **USART3 — `/dev/ttyACM0`, 115200 8N1**.

`BSP_COM_Init(COM1, ...)` in `main()` configures the port. `cli_serial_start()`
adds only the RX interrupt, which feeds a queue that `cliTask` blocks on. Dispatch
is a table of commands with argument counts, help text and handlers.

**`getchar` and `scanf` must not be used.** `_read` in `newlib_stubs.c` polls the
same USART and would race the ISR.

## Consequences

- One USB cable does flashing, debugging and console. No adapter, no extra wiring.
- **The console is scriptable**, which is what makes automated hardware verification
  possible at all — `status`, `lwip` and `device dump 10` are read by scripts, not just
  by a human.
- `cliTask` blocks on a queue instead of spinning, so an idle console costs nothing.
- Adding a command is one table row.
- **The stdio trap is permanent.** `_read` still exists for newlib's benefit and
  still polls the same peripheral. Anyone reaching for `scanf` reintroduces the race,
  and it presents as intermittently dropped characters. Recorded here and in
  `CLAUDE.md` because it is invisible at the call site.
- UART4 remains configured but unused.
- **The console is unauthenticated.** Physical USB access is full control of the
  hub. Accepted — see [security/threat-model.md](../security/threat-model.md).

## Alternatives rejected

**Keep UART4.** Needs an adapter and separate wiring for no benefit.

**Polled input with `getchar`.** Spins a task, races `_read`, and loses characters
under load.

**USB CDC on the H755's own USB peripheral.** A second USB stack to configure and
debug, replacing something the ST-Link already provides for free.

## See also

[network/ethernet.md](../network/ethernet.md)
