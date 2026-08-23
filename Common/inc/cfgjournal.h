/**
 * @file cfgjournal.h
 * @brief The ring's arithmetic: scanning it, replaying it, and placing the next record.
 *
 * No HAL and no flash writes, so the whole of it runs on the host against an
 * array. ADR-0027. radio_devices_docs/open_hub/arch/config-store.md
 */
#ifndef CFGJOURNAL_H
#define CFGJOURNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cfgstore.h"

#define CFG_SECTOR_NONE  0xFFu

/** Which of the two journal sectors, as an index rather than a flash number. */
enum {
    CFG_RING_A = 0,
    CFG_RING_B = 1
};

/** @brief Where a scan left the ring, and what it found. */
typedef struct cfg_scan {
    uint8_t  found;         /**< nonzero once a valid snapshot has been copied out */
    uint8_t  ring;          /**< CFG_RING_A or CFG_RING_B: the sector in use */
    uint8_t  dirty;         /**< the ring to erase at boot, or CFG_SECTOR_NONE */
    uint16_t snap_slot;     /**< where the winning snapshot starts */
    uint16_t next_slot;     /**< first free slot after the replayed tail */
    uint16_t deltas;        /**< records replayed on top of the snapshot */
    uint16_t damaged;       /**< slots the scan had to resync past */
    uint32_t seq;           /**< the newest sequence number seen anywhere */
    uint32_t snap_seq;      /**< the winning snapshot's own sequence number */
} cfg_scan_t;

/** What the next write must do, decided before any flash is touched. */
typedef enum {
    CFG_PLAN_DELTA = 0,   /**< append one slot at next_slot */
    CFG_PLAN_SNAPSHOT,    /**< a checkpoint is due and fits where it is due */
    CFG_PLAN_WRAP,        /**< switch rings and open the spare with a snapshot */
    CFG_PLAN_REFUSE       /**< the spare is dirty; this resolves on a reboot */
} cfg_plan_t;

/**
 * @brief CRC-32 as this store computes it, so the host and the board agree.
 * @param data  the bytes
 * @param len   how many
 * @return the checksum
 */
uint32_t cfg_crc32(const void *data, size_t len);

/**
 * @brief The checksum a record should carry: over everything after the crc field.
 * @param rec  the record, whose header states how many slots it occupies
 * @return the checksum
 */
uint32_t cfg_record_crc(const void *rec);

/**
 * @brief Stamps magic, version, type, slot count, seq and crc onto a record.
 * @param rec    the record to seal
 * @param type   CFG_T_*
 * @param slots  how many 128-byte slots it occupies
 * @param seq    its sequence number
 */
void cfg_record_seal(void *rec, uint8_t type, uint16_t slots, uint32_t seq);

/**
 * @brief Stamps a snapshot's entry: a one-slot record in every field but its magic.
 * @param ent   the entry inside a snapshot
 * @param type  CFG_T_*
 *
 * Deliberately sealed as if it were a record, so that the entry magic is the
 * single thing stopping a resync from reading it as one.
 */
void cfg_entry_seal(void *ent, uint8_t type);

/**
 * @brief Whether a record at a slot is intact and fits the space it claims.
 * @param rec    the candidate, at a slot boundary
 * @param avail  slots remaining in the sector from here
 * @retval 1  magic, version, slot count and checksum all hold
 * @retval 0  free, damaged, or claiming more room than there is
 */
int cfg_record_valid(const void *rec, uint16_t avail);

/**
 * @brief Reads both rings, copies out the newest snapshot and replays its tail.
 * @param ring_a  sector A, memory-mapped or an array
 * @param ring_b  sector B
 * @param out     receives the reconstructed image; untouched when nothing is found
 * @param scan    receives where the ring stands
 * @retval  0  a snapshot was found and replayed
 * @retval -1  neither ring holds one, which is an empty store and not a fault
 *
 * Nothing older than the winning snapshot is read, ever.
 */
int cfg_journal_scan(const void *ring_a, const void *ring_b,
                     cfg_snapshot_t *out, cfg_scan_t *scan);

/**
 * @brief Decides where the next record goes, given how far the ring has run.
 * @param scan          the state a scan left, updated by every write since
 * @param since_snap    deltas appended since the last checkpoint
 * @param spare_clean   nonzero when the other ring is erased and ready
 * @return which of the four things the writer must do
 */
cfg_plan_t cfg_journal_plan(const cfg_scan_t *scan, uint16_t since_snap,
                            int spare_clean);

/**
 * @brief Whether a slot has never been programmed.
 * @param slot  128 bytes at a slot boundary
 * @retval 1  every byte reads 0xFF
 * @retval 0  something is there, intact or not
 */
int cfg_slot_erased(const void *slot);

#endif /* CFGJOURNAL_H */
