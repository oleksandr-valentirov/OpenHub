# Dual-core split

**Status: implemented.**

## The split

| | CM7 (480 MHz) | CM4 (240 MHz) |
|---|---|---|
| Role | application | radio |
| Scheduling | FreeRTOS via CMSIS-RTOS v2 | bare superloop |
| Owns | Ethernet, lwIP, console, configuration | RFM69, TDMA timing, CRYP |
| Flash | `0x08000000` (bank 1) | `0x08100000` (bank 2) |
| Text size today | ~129 KB | ~32 KB |

The line is drawn by **timing class, not by workload size**. The radio runs a
TDMA slot grid where a missed deadline is a lost frame; the application runs a
network stack where jitter is invisible. Putting both under one scheduler would
mean either lwIP can delay a radio slot, or the radio has to run at a priority
that starves lwIP. Two cores make the question moot — see
[ADR-0001](../decisions/0001-dual-core-split.md).

That is also why CM4 has **no RTOS**. A superloop with a free-running
microsecond timer is easier to reason about against a slot grid than a preemptive
scheduler whose tick is coarser than the deadlines being met.

## Boot order

The order is load-bearing and easy to break:

1. Reset. **CM4 enters STOP immediately** and waits on `HSEM_ID_0`.
2. CM7 takes `HSEM_ID_0` and holds it across clock tree and peripheral init.
3. CM7 starts the FreeRTOS scheduler.
4. `StartDefaultTask` brings lwIP up, then **releases `HSEM_ID_0`**, waking CM4.

CM4 must not run while CM7 is still reconfiguring shared clocks, because the
RCC and the D2/D3 domains are common to both. The release point is deliberately
late.

**The trap:** `MX_LWIP_Init()` has to stay inside `StartDefaultTask`, not in
`main()`. lwIP in RTOS mode (`WITH_RTOS 1`) creates threads and mailboxes, which
requires a running scheduler. Moving it back into `main()` — a natural-looking
tidy-up — deadlocks the boot and leaves CM4 asleep forever, which presents as a
completely dead radio rather than as an lwIP fault.

## Watchdog

CM4 feeds IWDG2 from the radio superloop. A superloop that stops iterating is
exactly the failure the watchdog should catch, so the refresh sits in the loop
body and nowhere else — never in an interrupt, which would keep feeding a
watchdog for a core that is otherwise stuck.

Verified by the unchanged 1030 ms air cadence after IWDG2 was enabled: had the
refresh been missing, the core would have reset every 512 ms.

## Where crypto runs

Split by timing class again, and it is the reverse of what the core names
suggest: the **radio** core does only symmetric work, the **application** core
does the elliptic-curve maths. A P-256 scalar multiplication is tens of
milliseconds of software arithmetic and would blow a TDMA slot if it ever ran
inline on CM4.

See [security/crypto-architecture.md](../security/crypto-architecture.md) and
[ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md).

## See also

- [ipc.md](ipc.md) — how the two cores actually talk
- [memory-map.md](memory-map.md) — what each core can address
- [radio/README.md](../radio/README.md) — what CM4 spends its time on
