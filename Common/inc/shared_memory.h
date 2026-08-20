#pragma once

/* The cross-core mailbox lives in SRAM4 at the .shared_mem origin. Both cores
 * define the object and both linkers put it first in that section, so the
 * address agrees without either side hardcoding 0x38000000. (NOLOAD) on the
 * section keeps either core's startup from wiping what the other wrote.
 *
 * The message types themselves live in ipc.h. */
#define SHARED_MEM __attribute__((section(".shared_mem"), used, aligned(4)))
