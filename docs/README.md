# OpenHub documentation

Why the hub is built the way it is. Source code says *what* happens; these pages
say *why*, and what was rejected on the way.

Two kinds of page, deliberately kept apart:

- **System docs** (`architecture/`, `radio/`, `security/`, `network/`, `testing/`)
  describe how a subsystem works today. They are narrative and they are updated
  in place as the code changes.
- **[Decision records](decisions/)** capture one choice each, with the context that
  forced it and the alternatives that lost. They are dated and, once accepted,
  are not rewritten — a decision that no longer holds gets superseded by a new
  record rather than edited away.

System docs link down to the decisions behind them; decisions link back up to the
subsystem they shaped.

## Map

| Area | Start here | Covers |
|---|---|---|
| Architecture | [architecture/](architecture/) | core split, boot order, IPC, memory map, build |
| Radio | [radio/](radio/) | PHY, driver, TDMA, timebase, hopping, joining |
| Security | [security/](security/) | threat model, crypto placement, wire format, keys |
| Network | [network/](network/) | lwIP, addressing, planned TLS |
| Testing | [testing/](testing/) | SDR bench, host unit tests, on-target probing |
| Decisions | [decisions/](decisions/) | the ADR index |

## Reading order for someone new

1. [architecture/dual-core.md](architecture/dual-core.md) — the split everything else assumes.
2. [radio/README.md](radio/README.md) — the stack the project exists for.
3. [security/threat-model.md](security/threat-model.md) — what the crypto is defending against.
4. [testing/README.md](testing/README.md) — how any of it gets verified.

## Status vocabulary

Pages mark features so plans are not mistaken for working code:

- **Implemented** — in the firmware and exercised on hardware.
- **Implemented, unverified** — code exists, no hardware evidence yet.
- **Planned** — designed here, not written.

## Conventions

Prose in English, matching `CLAUDE.md` and the code comments. Numbers that came
from a measurement say so and name the tool; numbers that came from a datasheet
or a regulation cite it. An unattributed number in these pages is a bug.

`CLAUDE.md` in the repository root stays the short operational brief — build,
flash, traps. It is not replaced by this directory; it links into it.
