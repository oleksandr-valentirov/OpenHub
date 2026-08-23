/**
 * @file cfgstore.c
 * @brief The store above the flash: the boot scan, the RAM image, the appends.
 *
 * ADR-0027 §8. radio_devices_docs/open_hub/arch/config-store.md
 */
#include "cfgstoreapi.h"

#include <string.h>

#include "hsem_table.h"

static cfg_snapshot_t image;
static cfg_scan_t     scan;
static uint16_t       since_snap;
static uint32_t       next_seq = 1u;
static cfgflash_err_t boot_erase = CFGF_OK;
static uint32_t       boot_erase_ms;
static uint8_t        boot_erased;
static uint8_t        boot_sem_free;

static const void *ring_ptr(uint8_t ring)
{
    return (const void *)(ring == CFG_RING_A ? CFG_JOURNAL_ADDR_A
                                             : CFG_JOURNAL_ADDR_B);
}

int cfg_init(void)
{
    int rc;

    /* The post-condition of releasing HSEM_ID_0 in main().
     * radio_devices_docs/open_hub/arch/dual-core.md */
    boot_sem_free = (uint8_t)!HAL_HSEM_IsSemTaken(HSEM_ID_0);

    memset(&image, 0, sizeof(image));
    rc = cfg_journal_scan((const void *)CFG_JOURNAL_ADDR_A,
                          (const void *)CFG_JOURNAL_ADDR_B, &image, &scan);
    since_snap = scan.deltas;
    next_seq   = scan.seq + 1u;

    /* The one window where 954 ms costs nothing: CM4 released and on bank 2,
     * scheduler not started. radio_devices_docs/open_hub/arch/config-store.md */
    if (scan.dirty != CFG_SECTOR_NONE) {
        uint8_t sector = (scan.dirty == CFG_RING_A) ? CFG_JOURNAL_SECTOR_A
                                                    : CFG_JOURNAL_SECTOR_B;

        boot_erase = cfgflash_erase(sector, &boot_erase_ms);
        if (boot_erase == CFGF_OK) {
            scan.dirty  = CFG_SECTOR_NONE;
            boot_erased = 1;
        }
    }
    return rc;
}

cfgflash_err_t cfg_boot_erase(uint32_t *ms_out, uint8_t *ran)
{
    if (ms_out != NULL)
        *ms_out = boot_erase_ms;
    if (ran != NULL)
        *ran = boot_erased;
    return boot_erase;
}

int cfg_boot_erase_was_legal(void)
{
    return boot_sem_free;
}

const cfg_snapshot_t *cfg_image(void)   { return &image; }
const cfg_scan_t     *cfg_where(void)   { return &scan; }
uint16_t cfg_since_snapshot(void)       { return since_snap; }

int cfg_identity_read(cfg_identity_t *out)
{
    const uint8_t *base = (const uint8_t *)CFG_IDENTITY_ADDR;
    const cfg_identity_t *best = NULL;
    uint32_t best_seq = 0;

    for (uint16_t s = 0; s < CFG_SLOTS_PER_SECTOR; s++) {
        const uint8_t *p = base + (size_t)s * CFG_SLOT_BYTES;
        const cfg_hdr_t *h = (const cfg_hdr_t *)(const void *)p;

        if (cfg_slot_erased(p))
            break;
        if (!cfg_record_valid(p, (uint16_t)(CFG_SLOTS_PER_SECTOR - s)) ||
            h->type != CFG_T_IDENTITY)
            continue;
        if (best == NULL || h->seq >= best_seq) {
            best     = (const cfg_identity_t *)(const void *)p;
            best_seq = h->seq;
        }
    }
    if (best == NULL)
        return -1;
    if (out != NULL)
        *out = *best;
    return 0;
}

/* Append-only and never erased, so the next free slot is the first erased one.
 * radio_devices_docs/open_hub/arch/config-store.md */
static int identity_free_slot(uint16_t *out)
{
    const uint8_t *base = (const uint8_t *)CFG_IDENTITY_ADDR;

    for (uint16_t s = 0; s < CFG_SLOTS_PER_SECTOR; s++) {
        if (cfg_slot_erased(base + (size_t)s * CFG_SLOT_BYTES)) {
            *out = s;
            return 0;
        }
    }
    return -1;
}

cfgflash_err_t cfg_identity_write(const uint8_t priv[CFG_ROOT_KEY_BYTES],
                                  const uint8_t net[CFG_SESSION_BYTES])
{
    cfg_identity_t rec;
    cfg_identity_t have;
    uint16_t slot;
    uint32_t seq = 1u;

    if (identity_free_slot(&slot) != 0)
        return CFGF_ERR_RANGE;
    if (cfg_identity_read(&have) == 0)
        seq = have.hdr.seq + 1u;

    memset(&rec, 0, sizeof(rec));
    memcpy(rec.hub_priv, priv, CFG_ROOT_KEY_BYTES);
    memcpy(rec.net_key, net, CFG_SESSION_BYTES);
    cfg_record_seal(&rec, CFG_T_IDENTITY, 1u, seq);

    return cfgflash_program(CFG_IDENTITY_ADDR + (uint32_t)slot * CFG_SLOT_BYTES,
                            &rec, sizeof(rec));
}

/* The plan is decided before any flash is touched.
 * radio_devices_docs/open_hub/arch/config-store.md */
static cfgflash_err_t append(const void *rec, uint8_t type)
{
    cfg_plan_t plan;
    uint8_t spare = (scan.ring == CFG_RING_A) ? CFG_RING_B : CFG_RING_A;
    int spare_clean = cfg_slot_erased(ring_ptr(spare));
    uint32_t addr;
    cfgflash_err_t e;

    plan = cfg_journal_plan(&scan, since_snap, spare_clean);
    if (plan == CFG_PLAN_REFUSE)
        return CFGF_ERR_RANGE;

    if (plan == CFG_PLAN_WRAP) {
        /* Erasing the ring left behind is a boot's work, never a caller's. */
        scan.ring      = spare;
        scan.next_slot = 0;
        scan.found     = 1;
        plan = CFG_PLAN_SNAPSHOT;
    }

    if (plan == CFG_PLAN_SNAPSHOT) {
        cfg_record_seal(&image, CFG_T_SNAPSHOT, CFG_SNAP_SLOTS, next_seq);
        addr = (uint32_t)(uintptr_t)ring_ptr(scan.ring) +
               (uint32_t)scan.next_slot * CFG_SLOT_BYTES;
        e = cfgflash_program(addr, &image, sizeof(image));
        if (e != CFGF_OK)
            return e;
        scan.snap_slot = scan.next_slot;
        scan.snap_seq  = next_seq;
        scan.next_slot = (uint16_t)(scan.next_slot + CFG_SNAP_SLOTS);
        since_snap     = 0;
        next_seq++;
        return CFGF_OK;
    }

    (void)type;
    addr = (uint32_t)(uintptr_t)ring_ptr(scan.ring) +
           (uint32_t)scan.next_slot * CFG_SLOT_BYTES;
    e = cfgflash_program(addr, rec, CFG_SLOT_BYTES);
    if (e != CFGF_OK)
        return e;
    scan.next_slot++;
    since_snap++;
    next_seq++;
    return CFGF_OK;
}

/* The RAM image is the only truth at runtime; flash is how it survives a reset. */
static void image_put_device(const cfg_device_t *dev)
{
    cfg_device_t *slot = NULL;

    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        cfg_device_t *e = &image.dev[i];

        if (e->state != CFG_DEV_FREE && e->dev_id == dev->dev_id) {
            slot = e;
            break;
        }
        if (slot == NULL && e->state == CFG_DEV_FREE)
            slot = e;
    }
    if (slot == NULL)
        return;
    memcpy(slot, dev, sizeof(*slot));
    cfg_entry_seal(slot, CFG_T_DEV);
}

cfgflash_err_t cfg_put_device(const cfg_device_t *dev)
{
    cfg_device_t rec = *dev;

    cfg_record_seal(&rec, CFG_T_DEV, 1u, next_seq);
    image_put_device(dev);
    return append(&rec, CFG_T_DEV);
}

cfgflash_err_t cfg_put_config(const cfg_config_t *cfg)
{
    cfg_config_rec_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.cfg = *cfg;
    cfg_record_seal(&rec, CFG_T_CONFIG, 1u, next_seq);
    image.cfg = *cfg;
    return append(&rec, CFG_T_CONFIG);
}
