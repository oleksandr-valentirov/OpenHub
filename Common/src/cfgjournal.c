/**
 * @file cfgjournal.c
 * @brief The ring's arithmetic, with no HAL in it so the host can run all of it.
 *
 * ADR-0027. radio_devices_docs/open_hub/arch/config-store.md
 */
#include <string.h>

#include "cfgjournal.h"

uint32_t cfg_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;

    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

/* The crc covers everything after itself, so a record can be sealed in place. */
#define CFG_CRC_SKIP  (offsetof(cfg_hdr_t, crc) + sizeof(uint32_t))

uint32_t cfg_record_crc(const void *rec)
{
    const cfg_hdr_t *h = rec;
    size_t len = (size_t)h->slots * CFG_SLOT_BYTES;

    if (len <= CFG_CRC_SKIP)
        return 0;
    return cfg_crc32((const uint8_t *)rec + CFG_CRC_SKIP, len - CFG_CRC_SKIP);
}

void cfg_record_seal(void *rec, uint8_t type, uint16_t slots, uint32_t seq)
{
    cfg_hdr_t *h = rec;

    h->magic   = CFG_MAGIC;
    h->version = (uint8_t)CFG_VERSION;
    h->type    = type;
    h->slots   = slots;
    h->seq     = seq;
    h->crc     = cfg_record_crc(rec);
}

void cfg_entry_seal(void *ent, uint8_t type)
{
    cfg_hdr_t *h = ent;

    h->magic   = CFG_MAGIC_ENTRY;
    h->version = (uint8_t)CFG_VERSION;
    h->type    = type;
    h->slots   = 1u;
    h->seq     = 0u;
    h->crc     = cfg_record_crc(ent);
}

int cfg_slot_erased(const void *slot)
{
    const uint8_t *p = slot;

    for (uint32_t i = 0; i < CFG_SLOT_BYTES; i++) {
        if (p[i] != 0xFFu)
            return 0;
    }
    return 1;
}

int cfg_record_valid(const void *rec, uint16_t avail)
{
    const cfg_hdr_t *h = rec;

    if (h->magic != CFG_MAGIC || h->version != CFG_VERSION)
        return 0;
    if (h->slots == 0u || h->slots > avail)
        return 0;
    /* A record that claims a length no type of ours has is damaged, not future. */
    if (h->type == CFG_T_SNAPSHOT) {
        if (h->slots != CFG_SNAP_SLOTS)
            return 0;
    } else if (h->type == CFG_T_DEV || h->type == CFG_T_CONFIG ||
               h->type == CFG_T_IDENTITY) {
        if (h->slots != 1u)
            return 0;
    } else {
        return 0;
    }
    return h->crc == cfg_record_crc(rec);
}

static const uint8_t *slot_at(const void *ring, uint16_t slot)
{
    return (const uint8_t *)ring + (size_t)slot * CFG_SLOT_BYTES;
}

/* By dev_id, not by position: a snapshot's slot order is not a promise.
 * radio_devices_docs/open_hub/arch/config-store.md */
static void apply_dev(cfg_snapshot_t *img, const cfg_device_t *d)
{
    cfg_device_t *free_ent = NULL;

    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        cfg_device_t *e = &img->dev[i];

        if (e->state != CFG_DEV_FREE && e->dev_id == d->dev_id) {
            memcpy(e, d, sizeof(*e));
            cfg_entry_seal(e, CFG_T_DEV);
            return;
        }
        if (free_ent == NULL && e->state == CFG_DEV_FREE)
            free_ent = e;
    }
    /* A removal of a device not in the image has nothing to apply. */
    if (d->state == CFG_DEV_FREE || free_ent == NULL)
        return;
    memcpy(free_ent, d, sizeof(*free_ent));
    cfg_entry_seal(free_ent, CFG_T_DEV);
}

/* Steps by each record's own length; the first erased slot ends the tail.
 * radio_devices_docs/open_hub/arch/config-store.md */
static void walk(const void *ring, uint8_t which, cfg_scan_t *best,
                 const void **best_ring)
{
    uint16_t slot = 0;

    while (slot < CFG_SLOTS_PER_SECTOR) {
        const uint8_t *p = slot_at(ring, slot);
        const cfg_hdr_t *h = (const cfg_hdr_t *)(const void *)p;
        uint16_t avail = (uint16_t)(CFG_SLOTS_PER_SECTOR - slot);

        if (cfg_slot_erased(p))
            break;
        if (!cfg_record_valid(p, avail)) {
            best->damaged++;
            slot++;
            continue;
        }
        if (h->seq > best->seq)
            best->seq = h->seq;
        if (h->type == CFG_T_SNAPSHOT &&
            (!best->found || h->seq >= best->snap_seq)) {
            best->found = 1;
            best->ring  = which;
            best->snap_slot = slot;
            best->snap_seq = h->seq;
            *best_ring = ring;
        }
        slot = (uint16_t)(slot + h->slots);
    }
}

int cfg_journal_scan(const void *ring_a, const void *ring_b,
                     cfg_snapshot_t *out, cfg_scan_t *scan)
{
    const void *win = NULL;
    const void *other;
    uint16_t slot;

    memset(scan, 0, sizeof(*scan));
    scan->dirty = CFG_SECTOR_NONE;

    walk(ring_a, CFG_RING_A, scan, &win);
    walk(ring_b, CFG_RING_B, scan, &win);

    if (!scan->found) {
        scan->next_slot = 0;
        return -1;
    }

    memcpy(out, slot_at(win, scan->snap_slot), sizeof(*out));

    /* The tail: every record after the winning snapshot, in the same ring. */
    slot = (uint16_t)(scan->snap_slot + CFG_SNAP_SLOTS);
    while (slot < CFG_SLOTS_PER_SECTOR) {
        const uint8_t *p = slot_at(win, slot);
        const cfg_hdr_t *h = (const cfg_hdr_t *)(const void *)p;
        uint16_t avail = (uint16_t)(CFG_SLOTS_PER_SECTOR - slot);

        if (cfg_slot_erased(p))
            break;
        if (!cfg_record_valid(p, avail)) {
            slot++;
            continue;
        }
        if (h->type == CFG_T_DEV)
            apply_dev(out, (const cfg_device_t *)(const void *)p);
        else if (h->type == CFG_T_CONFIG)
            memcpy(&out->cfg, &((const cfg_config_rec_t *)(const void *)p)->cfg,
                   sizeof(out->cfg));
        scan->deltas++;
        slot = (uint16_t)(slot + h->slots);
    }
    scan->next_slot = slot;

    /* The ring left behind at the last wrap, still holding the older log. */
    other = (win == ring_a) ? ring_b : ring_a;
    if (!cfg_slot_erased(slot_at(other, 0)))
        scan->dirty = (win == ring_a) ? CFG_RING_B : CFG_RING_A;

    return 0;
}

cfg_plan_t cfg_journal_plan(const cfg_scan_t *scan, uint16_t since_snap,
                            int spare_clean)
{
    uint16_t left;
    int due;

    if (!scan->found)
        return spare_clean ? CFG_PLAN_WRAP : CFG_PLAN_REFUSE;

    left = (uint16_t)(CFG_SLOTS_PER_SECTOR - scan->next_slot);
    due  = (since_snap >= CFG_SNAP_EVERY);

    if (due) {
        if (left >= CFG_SNAP_SLOTS)
            return CFG_PLAN_SNAPSHOT;
        return spare_clean ? CFG_PLAN_WRAP : CFG_PLAN_REFUSE;
    }
    if (left >= 1u)
        return CFG_PLAN_DELTA;
    return spare_clean ? CFG_PLAN_WRAP : CFG_PLAN_REFUSE;
}
