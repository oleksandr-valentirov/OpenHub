# Architecture

The hub is a NUCLEO-H755ZI-Q: one STM32H755 with a Cortex-M7 and a Cortex-M4
sharing flash, RAM and peripherals. Almost every structural decision in the
project follows from splitting work across those two cores.

| Page | Subject |
|---|---|
| [dual-core.md](dual-core.md) | which core owns what, and the boot handshake |
| [ipc.md](ipc.md) | hardware semaphores and the shared-memory mailbox |
| [keystore.md](keystore.md) | the flash store and the durable superframe counter | counter done |
| [memory-map.md](memory-map.md) | flash and RAM layout, the RAM_D2 hole, MPU |
| [build-and-generation.md](build-and-generation.md) | CubeMX as source of truth, CMake, linker scripts |

Decisions: [ADR-0001](../decisions/0001-dual-core-split.md),
[ADR-0002](../decisions/0002-sram4-mailbox.md),
[ADR-0003](../decisions/0003-cubemx-source-of-truth.md),
[ADR-0004](../decisions/0004-reference-libraries-not-vendor.md),
[ADR-0013](../decisions/0013-crypto-peripheral-ownership.md).
