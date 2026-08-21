#pragma once

/**
 * @file shared_memory.h
 * @brief Places the cross-core mailbox in SRAM4, at whatever origin the linker gives it.
 *
 * Neither core carries 0x38000000 as a literal, so the region moves in one place
 * per core. radio_devices_docs/open_hub/arch/ipc.md
 */
#define SHARED_MEM __attribute__((section(".shared_mem"), used, aligned(4)))
