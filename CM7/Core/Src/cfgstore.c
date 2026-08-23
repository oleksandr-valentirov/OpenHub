/**
 * @file cfgstore.c
 * @brief The store above the flash: the boot scan, the RAM image, the appends.
 *
 * ADR-0027 §8. radio_devices_docs/open_hub/arch/config-store.md
 */
#include "cfgstoreapi.h"

#include <string.h>

#include "hsem_table.h"
#include "keystore.h"

static cfg_snapshot_t image;
static cfg_scan_t     scan;
static uint16_t       since_snap;
static uint32_t       next_seq = 1u;
static cfgflash_err_t boot_erase = CFGF_OK;
static uint32_t       boot_erase_ms;
static uint8_t        boot_erased;
static uint8_t        boot_sem_free;
/* Which store answered last; a fallback can cover a broken new path.
 * radio_devices_docs/open_hub/arch/config-store.md */
static cfg_src_t      key_source = CFG_SRC_NONE;
static uint8_t        opened;
static uint32_t       open_carried;
static uint32_t       open_erase_ms;
static cfgflash_err_t open_erase = CFGF_OK;
static cfgflash_err_t open_err   = CFGF_OK;

static void open_ring(void);

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

    /* Where 954 ms is free: CM4 released and on bank 2, scheduler not started.
     * radio_devices_docs/open_hub/arch/config-store.md */
    if (scan.dirty != CFG_SECTOR_NONE) {
        uint8_t sector = (scan.dirty == CFG_RING_A) ? CFG_JOURNAL_SECTOR_A
                                                    : CFG_JOURNAL_SECTOR_B;

        boot_erase = cfgflash_erase(sector, &boot_erase_ms);
        if (boot_erase == CFGF_OK) {
            scan.dirty  = CFG_SECTOR_NONE;
            boot_erased = 1;
        }
    }
    /* An identity to anchor a store, and no store: open one. True exactly once
     * per board. radio_devices_docs/open_hub/arch/config-store.md */
    if (rc != 0 && cfg_identity_read(NULL) == 0) {
        open_ring();
        if (opened)
            rc = 0;
    }

    /* The old log shares these sectors and must stop touching them.
     * radio_devices_docs/open_hub/arch/config-store.md */
    if (rc == 0)
        ks_retire();
    return rc;
}

/* §10 step 1: the live roster out of the old log; the cache excludes tombstones.
 * radio_devices_docs/open_hub/arch/config-store.md */
static uint32_t carry_roster(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < ks_count() && n < CFG_DEVICE_MAX; i++) {
        const ks_record_t *r = ks_at(i);
        cfg_device_t *e;

        if (r == NULL || r->type != KS_TYPE_DEVICE ||
            r->state == KS_STATE_DELETED || r->state == KS_STATE_FREE)
            continue;
        e = &image.dev[n++];
        memset(e, 0, sizeof(*e));
        e->dev_id       = r->dev_id;
        e->key_gen      = r->key_gen;
        e->rotate_epoch = r->rotate_epoch;
        e->rx_floor     = r->rx_floor;
        e->tx_floor     = r->tx_floor;
        e->slot         = r->slot;
        e->state        = (r->state == KS_STATE_PAIRED) ? CFG_DEV_PAIRED
                                                        : CFG_DEV_ENROLLED;
        memcpy(e->pubkey, r->pubkey, CFG_PUBKEY_BYTES);
        memcpy(e->root_key, r->root_key, CFG_ROOT_KEY_BYTES);
        memcpy(e->session_key, r->session_key, CFG_SESSION_BYTES);
        memcpy(e->last_nonce, r->last_nonce, CFG_NONCE_BYTES);
        cfg_entry_seal(e, CFG_T_DEV);
    }
    return n;
}

/* Ring B, so ring A stays readable until a later boot reclaims it.
 * radio_devices_docs/open_hub/arch/config-store.md */
static void open_ring(void)
{
    if (!cfgflash_is_erased(CFG_JOURNAL_ADDR_B, CFG_SNAP_BYTES)) {
        open_erase = cfgflash_erase(CFG_JOURNAL_SECTOR_B, &open_erase_ms);
        if (open_erase != CFGF_OK)
            return;
    }
    open_carried = carry_roster();
    cfg_record_seal(&image, CFG_T_SNAPSHOT, CFG_SNAP_SLOTS, 1u);
    open_err = cfgflash_program(CFG_JOURNAL_ADDR_B, &image, sizeof(image));
    if (open_err != CFGF_OK)
        return;

    scan.found     = 1;
    scan.ring      = CFG_RING_B;
    scan.snap_slot = 0;
    scan.snap_seq  = 1u;
    scan.next_slot = CFG_SNAP_SLOTS;
    scan.deltas    = 0;
    since_snap     = 0;
    next_seq       = 2u;
    opened         = 1;
    /* Ring A is the ring left behind now, and the next boot reclaims it. */
    if (!cfg_slot_erased((const void *)CFG_JOURNAL_ADDR_A))
        scan.dirty = CFG_RING_A;
}

cfgflash_err_t cfg_open_result(uint32_t *carried, uint32_t *erase_ms,
                               cfgflash_err_t *erase_rc)
{
    if (carried != NULL)
        *carried = open_carried;
    if (erase_ms != NULL)
        *erase_ms = open_erase_ms;
    if (erase_rc != NULL)
        *erase_rc = open_erase;
    return opened ? CFGF_OK : open_err;
}

int cfg_ring_was_opened(void)
{
    return opened;
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

/* The identity moved to its own sector; the old log is the fallback until it is
 * erased. radio_devices_docs/open_hub/arch/config-store.md */
int hub_key_get(uint8_t priv[CFG_ROOT_KEY_BYTES])
{
    if (cfg_hub_key_get(priv) == 0) {
        key_source = CFG_SRC_STORE;
        return 0;
    }
    if (ks_hub_key_get(priv) == 0) {
        key_source = CFG_SRC_OLD_LOG;
        return 0;
    }
    key_source = CFG_SRC_NONE;
    return -1;
}

cfg_src_t hub_key_source(void)
{
    return key_source;
}

/* Never creates one, unlike the old log's accessor, so a read stays a read. */
int hub_net_key_get(uint8_t key[CFG_SESSION_BYTES])
{
    if (cfg_net_key_get(key) == 0)
        return 0;
    return ks_net_key_get(key);
}

int cfg_hub_key_get(uint8_t priv[CFG_ROOT_KEY_BYTES])
{
    cfg_identity_t id;

    if (cfg_identity_read(&id) != 0)
        return -1;
    memcpy(priv, id.hub_priv, CFG_ROOT_KEY_BYTES);
    memset(&id, 0, sizeof(id));
    return 0;
}

int cfg_net_key_get(uint8_t key[CFG_SESSION_BYTES])
{
    cfg_identity_t id;

    if (cfg_identity_read(&id) != 0)
        return -1;
    memcpy(key, id.net_key, CFG_SESSION_BYTES);
    memset(&id, 0, sizeof(id));
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

const cfg_device_t *cfg_find(uint32_t dev_id)
{
    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        if (image.dev[i].state != CFG_DEV_FREE && image.dev[i].dev_id == dev_id)
            return &image.dev[i];
    }
    return NULL;
}

const cfg_device_t *cfg_at(uint32_t i)
{
    return (i < CFG_DEVICE_MAX) ? &image.dev[i] : NULL;
}

uint32_t cfg_live_devices(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++)
        if (image.dev[i].state != CFG_DEV_FREE)
            n++;
    return n;
}

/* Counted over the image, which is the whole roster - the old store's cache was
 * not. radio_devices_docs/open_hub/arch/config-store.md */
static int lowest_free_slot(uint32_t skip_dev_id, uint8_t *out)
{
    for (uint32_t s = 0; s < CFG_DEVICE_MAX && s < RADIO_DEVICE_MAX; s++) {
        uint32_t i;

        for (i = 0; i < CFG_DEVICE_MAX; i++) {
            const cfg_device_t *e = &image.dev[i];

            if (e->state == CFG_DEV_FREE || e->dev_id == skip_dev_id)
                continue;
            if (e->slot == (uint8_t)s)
                break;
        }
        if (i == CFG_DEVICE_MAX) {
            *out = (uint8_t)s;
            return 0;
        }
    }
    return -1;
}

cfgflash_err_t cfg_enrol(uint32_t dev_id, uint8_t *slot_out)
{
    const cfg_device_t *have = cfg_find(dev_id);
    cfg_device_t d;
    uint8_t slot;

    if (dev_id == 0u)
        return CFGF_ERR_ALIGN;
    if (have != NULL) {
        d    = *have;
        slot = d.slot;
        d.key_gen++;
        memset(d.root_key, 0, sizeof(d.root_key));
        memset(d.session_key, 0, sizeof(d.session_key));
    } else {
        if (cfg_live_devices() >= CFG_DEVICE_MAX ||
            lowest_free_slot(dev_id, &slot) != 0)
            return CFGF_ERR_RANGE;
        memset(&d, 0, sizeof(d));
        d.dev_id  = dev_id;
        d.slot    = slot;
        d.key_gen = 1u;
    }
    d.state = CFG_DEV_ENROLLED;
    if (slot_out != NULL)
        *slot_out = slot;
    return cfg_put_device(&d);
}

cfgflash_err_t cfg_forget(uint32_t dev_id)
{
    const cfg_device_t *have = cfg_find(dev_id);
    cfg_device_t d;

    if (have == NULL)
        return CFGF_ERR_RANGE;
    d = *have;
    /* A removal frees the entry rather than spending one.
     * radio_devices_docs/open_hub/arch/config-store.md */
    memset(&d.pubkey, 0, sizeof(d.pubkey));
    memset(&d.root_key, 0, sizeof(d.root_key));
    memset(&d.session_key, 0, sizeof(d.session_key));
    d.state = CFG_DEV_FREE;
    return cfg_put_device(&d);
}

cfgflash_err_t cfg_pair_complete(uint32_t dev_id, const uint8_t session[16],
                                 const uint8_t nonce[8], uint32_t rotate_epoch,
                                 const uint8_t pubkey[CFG_PUBKEY_BYTES])
{
    const cfg_device_t *have = cfg_find(dev_id);
    cfg_device_t d;

    if (have == NULL)
        return CFGF_ERR_RANGE;
    d = *have;
    d.state        = CFG_DEV_PAIRED;
    d.rotate_epoch = rotate_epoch;
    d.rx_floor     = 0;
    memcpy(d.session_key, session, CFG_SESSION_BYTES);
    memcpy(d.last_nonce, nonce, CFG_NONCE_BYTES);
    memcpy(d.pubkey, pubkey, CFG_PUBKEY_BYTES);
    return cfg_put_device(&d);
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
