# Memory map

**Status: implemented.**

## Flash

| Region | Start | Size | Contents |
|---|---|---|---|
| Bank 1 | `0x08000000` | 1 MB | CM7 image (~129 KB used) |
| Bank 2 | `0x08100000` | 1 MB | CM4 image (~32 KB used) |

Writing an ELF with STM32_Programmer_CLI picks the address up from the file, so
the two are flashed with the same command shape and no `-a` argument.

Headroom is comfortable on both banks. It is worth stating plainly because the
[mbedTLS decision](../decisions/0011-mbedtls-on-cm7-only.md) turns on it: a TLS
build lands in bank 1 with several hundred KB to spare.

## RAM

| Region | Start | Domain | Use |
|---|---|---|---|
| DTCM | `0x20000000` | — | CM7 stacks and hot data |
| AXI SRAM (D1) | `0x24000000` | D1 | CM7 heap and `.bss` |
| SRAM1/2/3 (D2) | `0x30000000` | D2 | Ethernet descriptors, lwIP pools |
| SRAM4 (D3) | `0x38000000` | D3 | `.shared_mem` — the cross-core mailbox |
| Backup SRAM | `0x38800000` | D3 | unused |

## The RAM_D2 hole

The map reports RAM_D2 at about 95% used. **Most of that is not data.**

`.lwip_sec` starts at the region origin and the linker script then jumps the
location counter to `0x30040000`, so roughly 256 KB of the section is fill. The
real content is the Ethernet descriptors and a ~19 KB RX pool.

The jump exists because the MPU opens a non-cacheable window at `0x30040000`,
and the descriptors have to live inside it — the ETH DMA and the CPU must see
the same bytes.

Two consequences worth carrying:

- **`0x30000000`..`0x30020000` is genuinely free.** The usage figure should not
  discourage anyone from using it.
- **The lwIP heap is placed by a raw address.** `LWIP_RAM_HEAP_POINTER` is
  `0x30020000`, a constant the linker knows nothing about. Nothing stops a future
  section from being laid down on top of it, and nothing would warn. Moving that
  heap into a real linker-defined symbol is outstanding work.

## Descriptor section names

`custom_m7_flash.ld` must spell the sections exactly as `ethernetif.c` emits
them: **`.RxDescripSection` / `.TxDescripSection`**.

The widely-copied ST community article spells them without the `s`. With that
spelling nothing fails to link — the descriptors quietly land in cacheable RAM_D1
instead of the non-cacheable window, and Ethernet misbehaves in ways that look
like a driver bug.

## Linker scripts

The build links `custom_m4_flash.ld` and `custom_m7_flash.ld` through
`STM32_LINKER_SCRIPT`, **not** the `stm32h755xx_*.ld` files CubeMX writes. Custom
sections (`.lwip_sec`, `.shared_mem`, `RAM_D3`) live in the custom files; editing
the generated ones has no effect on the build and misleads the next reader.

## See also

- [ipc.md](ipc.md) — why `.shared_mem` is `(NOLOAD)`
- [build-and-generation.md](build-and-generation.md) — what CubeMX owns here
- [network/ethernet.md](../network/ethernet.md) — the lwIP side of RAM_D2
