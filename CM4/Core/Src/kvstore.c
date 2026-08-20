/* Durable storage for the superframe counter.
 *
 * The counter is the GCM nonce's most significant field, so a hub that restarts
 * it at zero and reuses a persisted session key repeats nonces - which does not
 * merely leak plaintext, it leaks the authentication subkey. Until this existed,
 * a hub reboot had to force re-pairing.
 *
 * **A ceiling is stored, not the counter.** Writing 32 bytes every 2 s would
 * wear the flash out and cost a bus stall in the middle of the slot grid.
 * Instead a value KV_RESERVE_AHEAD superframes in the future is written, and
 * the next boot starts there. Nothing at or below a stored ceiling is ever
 * reused, at one write per KV_RESERVE_AHEAD superframes.
 *
 * The cost of an unclean shutdown is skipping up to KV_RESERVE_AHEAD counter
 * values. That is free: the space is 2^32 and skipping is the conservative
 * direction, the same argument key-lifecycle.md makes for the transmit floor. */

#include <string.h>

#include "main.h"
#include "kvstore.h"

#define KV_MAGIC            0x564B484Fu   /* 'OHKV' little-endian */
#define KV_VERSION          1u
#define KV_TYPE_COUNTER     1u

/* 2.3 hours between writes, and the counter jumps by at most this across a
 * reboot. Devices see that as a forward jump; a forward jump is the direction a
 * replay check accepts, and a device whose plausibility check refuses one that
 * large recovers by re-taking a beacon on trust. */
#ifndef KV_RESERVE_AHEAD                 /* overridable so the write path can be
                                          * stress-tested at a rate a bench can
                                          * watch; 4096 is one write per 2.3 h */
#define KV_RESERVE_AHEAD    4096u
#endif

/* Sectors 6 and 7 of bank 2. CM4's code lives in sector 0 and reaches 40 KB, so
 * the top of the bank is free and is not going to grow into these. */
#define KV_SECTOR_A         FLASH_SECTOR_6
#define KV_SECTOR_B         FLASH_SECTOR_7
#define KV_ADDR_A           0x081C0000u
#define KV_ADDR_B           0x081E0000u
#define KV_SECTOR_BYTES     0x20000u
#define KV_RECORD_BYTES     32u
#define KV_SLOTS_PER_SECTOR (KV_SECTOR_BYTES / KV_RECORD_BYTES)

/* One H7 flash word. A flash word can be programmed once between erases, so a
 * record is written whole and never revised. */
typedef struct kv_record {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t pad;
    uint32_t seq;           /* highest valid seq wins; survives the sector swap */
    uint32_t counter_mark;  /* nothing at or below this may be reused */
    /* Carried now although pairing does not exist yet, because the format has
     * to stop changing before anything depends on it - and because a floor that
     * names its generation is *detectably* stale rather than merely cleared at
     * the right moment. Clearing by call-site works until someone moves it. */
    uint32_t key_gen;
    uint32_t rx_floor;      /* replay floor, scoped to key_gen, never to a device */
    uint32_t spare;
    uint32_t crc;
} kv_record_t;

/* A record must fill a flash word exactly. Not "be at most 32 bytes": a short
 * record leaves filler a future field could quietly claim, and a long one is two
 * programs where the code performs one. This is the invariant that actually
 * breaks if someone adds a field carelessly, so it is the one asserted. */
_Static_assert(sizeof(kv_record_t) == KV_RECORD_BYTES,
               "kv_record_t must fill exactly one H7 flash word");

static uint32_t active_addr;
static uint32_t next_slot;
static uint32_t reserved;
static uint32_t last_seq;
static uint32_t writes;
static uint32_t errors;
static uint8_t  spare_erased;
static uint8_t  exhausted;
static uint8_t  ready;

/* Bitwise rather than table-driven: this runs over at most 8192 records once at
 * boot, and a 1 KB table is worth more than the milliseconds it saves. */
static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;

    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

static uint32_t record_crc(const kv_record_t *r) {
    return crc32(r, offsetof(kv_record_t, crc));
}

static int record_valid(const kv_record_t *r) {
    return r->magic == KV_MAGIC && r->version == KV_VERSION &&
           r->type == KV_TYPE_COUNTER && r->crc == record_crc(r);
}

static int slot_erased(const kv_record_t *r) {
    const uint32_t *w = (const uint32_t *)(const void *)r;

    for (unsigned i = 0; i < KV_RECORD_BYTES / 4u; i++)
        if (w[i] != 0xFFFFFFFFu)
            return 0;
    return 1;
}

static uint8_t erase_sector(uint32_t sector) {
    FLASH_EraseInitTypeDef e;
    uint32_t bad = 0;
    HAL_StatusTypeDef st;

    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.Banks        = FLASH_BANK_2;
    e.Sector       = sector;
    e.NbSectors    = 1;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return 1;
    st = HAL_FLASHEx_Erase(&e, &bad);
    (void)HAL_FLASH_Lock();
    return (st == HAL_OK) ? 0 : 1;
}

static uint8_t write_record(uint32_t addr, const kv_record_t *r) {
    HAL_StatusTypeDef st;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return 1;
    /* On H7 the third argument is the *address* of the 32 bytes, not the data. */
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr, (uint64_t)(uint32_t)r);
    (void)HAL_FLASH_Lock();
    return (st == HAL_OK) ? 0 : 1;
}

/* Newest valid record across both sectors, and where the next one goes. */
/* Newest valid record, and the append point.
 *
 * The append point is the first *erased* slot, never one past the last valid
 * record. Those differ whenever a slot holds something the scanner rejects - a
 * torn write, or records of an older format - and deriving it from valid
 * records alone aims the next write at occupied flash.
 *
 * The consequence here is the worst in the project. An H7 flash word cannot be
 * programmed twice, so the write fails, `exhausted` latches, kv_counter_safe()
 * goes false and the radio stops transmitting - correctly, since an unreserved
 * counter is nonce reuse. But a reboot does not clear it: the boot erase cleans
 * the *spare*, the active sector keeps the torn record, and the same append
 * point is computed again. One torn record would silence the hub permanently.
 *
 * Found on the device side, which has the same store shape and where the same
 * bug instead fell through to a page erase on every write. */
static void scan(void) {
    uint32_t best_seq = 0;
    uint32_t first_erased[2];
    int found = 0;

    active_addr = KV_ADDR_A;
    next_slot   = 0;
    reserved    = 0;
    last_seq    = 0;

    for (int s = 0; s < 2; s++) {
        uint32_t base = s ? KV_ADDR_B : KV_ADDR_A;

        first_erased[s] = KV_SLOTS_PER_SECTOR;
        for (uint32_t i = 0; i < KV_SLOTS_PER_SECTOR; i++) {
            const kv_record_t *r =
                (const kv_record_t *)(base + i * KV_RECORD_BYTES);

            if (slot_erased(r)) {
                first_erased[s] = i;
                break;              /* the log is written in order */
            }
            if (!record_valid(r))
                continue;           /* torn, or an older format; skip, do not stop */
            if (!found || (int32_t)(r->seq - best_seq) > 0) {
                best_seq    = r->seq;
                reserved    = r->counter_mark;
                active_addr = base;
                found       = 1;
            }
        }
    }

    last_seq = best_seq;
    if (!found && first_erased[0] >= KV_SLOTS_PER_SECTOR
               && first_erased[1] < KV_SLOTS_PER_SECTOR)
        active_addr = KV_ADDR_B;
    next_slot = first_erased[(active_addr == KV_ADDR_B) ? 1 : 0];
}

uint8_t kv_init(void) {
    uint32_t spare;

    scan();
    spare = (active_addr == KV_ADDR_A) ? KV_ADDR_B : KV_ADDR_A;

    /* Erase the spare now, at boot, where a stall is harmless. A 128 KB erase
     * can outlast the 512 ms watchdog and stalls the bank CM4 executes from, so
     * it must never happen while the slot grid is running. Keeping a spare
     * always ready turns an in-service erase into an in-service *write*. */
    spare_erased = 1;
    for (uint32_t i = 0; i < KV_SLOTS_PER_SECTOR; i++) {
        if (!slot_erased((const kv_record_t *)(spare + i * KV_RECORD_BYTES))) {
            spare_erased = 0;
            break;
        }
    }
    if (!spare_erased) {
        uint32_t sector = (spare == KV_ADDR_A) ? KV_SECTOR_A : KV_SECTOR_B;

        if (erase_sector(sector) == 0)
            spare_erased = 1;
        else
            errors++;
    }

    exhausted = 0;
    ready = 1;
    return 0;
}

uint32_t kv_reserved(void) { return reserved; }

/* Test scaffolding: an unreadable record occupying a slot.
 *
 * A record here is exactly one flash word and the H7 programs 256 bits or
 * fails, so a *torn* record in the multi-word sense is not reachable on this
 * store - unlike CM7's, which is four words. What is reachable is a slot the
 * scanner cannot accept: corruption, or a record of a format this build does
 * not know. Both present identically, as a failed CRC.
 *
 * The seq is HIGHER than any valid record, so a scanner that accepted it would
 * win the comparison and adopt its counter mark. One with a low seq would be
 * ignored for the wrong reason and prove nothing. */
int kv_write_torn(void) {
    kv_record_t r __attribute__((aligned(32)));
    uint32_t addr;
    HAL_StatusTypeDef st;

    if (!ready || exhausted || next_slot >= KV_SLOTS_PER_SECTOR)
        return -1;

    memset(&r, 0, sizeof(r));
    r.magic        = KV_MAGIC;
    r.version      = KV_VERSION;
    r.type         = KV_TYPE_COUNTER;
    r.seq          = last_seq + 1000u;
    r.counter_mark = 0xDEADBEEFu;    /* would be catastrophic if adopted */
    r.crc          = record_crc(&r);

    addr = active_addr + next_slot * KV_RECORD_BYTES;
    r.crc ^= 0xFFFFFFFFu;       /* what an unreadable slot looks like */
    if (HAL_FLASH_Unlock() != HAL_OK)
        return -1;
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr, (uint64_t)(uint32_t)&r);
    (void)HAL_FLASH_Lock();
    if (st != HAL_OK)
        return -1;

    next_slot++;    /* it occupies its slot and must not be reused */
    return 0;
}

uint8_t kv_counter_safe(uint32_t counter) {
    if (!ready)
        return 0;
    return ((int32_t)(counter - reserved) < 0) ? 1u : 0u;
}
uint32_t kv_writes(void)   { return writes; }
uint32_t kv_errors(void)   { return errors; }

uint32_t kv_slots_left(void) {
    uint32_t left = KV_SLOTS_PER_SECTOR - next_slot;

    return spare_erased ? left + KV_SLOTS_PER_SECTOR : left;
}

uint8_t kv_reserve(uint32_t counter) {
    kv_record_t r __attribute__((aligned(32)));
    uint32_t addr;

    if (!ready || exhausted)
        return 1;
    /* Signed difference: the counter wraps, and so does the mark with it. */
    if ((int32_t)(counter - reserved) < 0)
        return 0;                       /* still inside what flash guarantees */

    if (next_slot >= KV_SLOTS_PER_SECTOR) {
        if (!spare_erased) {
            /* Both sectors full. Refusing to write is only half the answer:
             * the counter goes on advancing past the ceiling regardless, and a
             * future boot would then hand those values out again. So the
             * refusal has to be *acted on* - kv_counter_safe() goes false and
             * the radio stops transmitting. An earlier version of this comment
             * called refusing "the safe direction" while nothing acted on it,
             * which made the store's whole purpose conditional on a path that
             * had never run. A reboot compacts and clears it. */
            errors++;
            exhausted = 1;   /* latch: the caller runs every superloop pass, and
                              * retrying a write that cannot succeed hammers the
                              * flash unlock path thousands of times a second.
                              * Measured at 1.79M attempts in 45 s before this. */
            return 1;
        }
        active_addr  = (active_addr == KV_ADDR_A) ? KV_ADDR_B : KV_ADDR_A;
        next_slot    = 0;
        spare_erased = 0;               /* the old sector is erased at next boot */
    }

    memset(&r, 0, sizeof(r));
    r.magic        = KV_MAGIC;
    r.version      = KV_VERSION;
    r.type         = KV_TYPE_COUNTER;
    r.seq          = ++last_seq;
    r.counter_mark = counter + KV_RESERVE_AHEAD;
    r.crc          = record_crc(&r);

    addr = active_addr + next_slot * KV_RECORD_BYTES;
    if (write_record(addr, &r) != 0) {
        errors++;
        exhausted = 1;   /* a flash that refuses a write will refuse the next one */
        last_seq--;
        return 1;
    }
    next_slot++;
    writes++;
    reserved = r.counter_mark;
    return 0;
}
