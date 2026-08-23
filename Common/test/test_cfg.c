/* The ring of checkpoints, exercised against an array rather than a board.
 * ADR-0027. radio_devices_docs/open_hub/arch/config-store.md */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "cfgjournal.h"

static uint8_t ring[2][CFG_SECTOR_BYTES];
static int failures;

#define CHECK(cond, ...) do {                                        \
        if (!(cond)) {                                               \
            printf("  FAIL %s:%d: ", __func__, __LINE__);            \
            printf(__VA_ARGS__); printf("\n"); failures++;           \
        }                                                            \
    } while (0)

static void erase_all(void)
{
    memset(ring, 0xFF, sizeof(ring));
}

static void *slot_ptr(int r, uint16_t slot)
{
    return &ring[r][(size_t)slot * CFG_SLOT_BYTES];
}

/* A snapshot whose roster holds `n` devices with ids 0x100..0x100+n-1. */
static void put_snapshot(int r, uint16_t slot, uint32_t seq, uint32_t n,
                         uint32_t telem_ip)
{
    cfg_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.cfg.telem_ip = telem_ip;
    for (uint32_t i = 0; i < n && i < CFG_DEVICE_MAX; i++) {
        s.dev[i].dev_id = 0x100u + i;
        s.dev[i].state  = CFG_DEV_ENROLLED;
        s.dev[i].slot   = (uint8_t)i;
        cfg_entry_seal(&s.dev[i], CFG_T_DEV);
    }
    cfg_record_seal(&s, CFG_T_SNAPSHOT, CFG_SNAP_SLOTS, seq);
    memcpy(slot_ptr(r, slot), &s, sizeof(s));
}

static void put_dev(int r, uint16_t slot, uint32_t seq, uint32_t dev_id,
                    uint8_t state, uint8_t uplink)
{
    cfg_device_t d;

    memset(&d, 0, sizeof(d));
    d.dev_id = dev_id;
    d.state  = state;
    d.slot   = uplink;
    cfg_record_seal(&d, CFG_T_DEV, 1, seq);
    memcpy(slot_ptr(r, slot), &d, sizeof(d));
}

static uint32_t live_devices(const cfg_snapshot_t *s)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        if (s->dev[i].state != CFG_DEV_FREE)
            n++;
    }
    return n;
}

/* Regardless of state, which find_dev() cannot do. */
static int id_present(const cfg_snapshot_t *s, uint32_t dev_id)
{
    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        if (s->dev[i].dev_id == dev_id)
            return 1;
    }
    return 0;
}

/* Entries nothing has ever been written into: all zero, magic included. */
static uint32_t untouched_entries(const cfg_snapshot_t *s)
{
    static const cfg_device_t zero;
    uint32_t n = 0;

    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        if (memcmp(&s->dev[i], &zero, sizeof(zero)) == 0)
            n++;
    }
    return n;
}

static const cfg_device_t *find_dev(const cfg_snapshot_t *s, uint32_t dev_id)
{
    for (uint32_t i = 0; i < CFG_DEVICE_MAX; i++) {
        if (s->dev[i].state != CFG_DEV_FREE && s->dev[i].dev_id == dev_id)
            return &s->dev[i];
    }
    return NULL;
}

/* An empty store is a usable one, and must not read as a fault. */
static void test_empty(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;

    erase_all();
    memset(&img, 0xAA, sizeof(img));
    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == -1, "empty scan found something");
    CHECK(sc.found == 0, "found set on an empty ring");
    CHECK(sc.dirty == CFG_SECTOR_NONE, "an empty ring has nothing to erase");
    CHECK(cfg_journal_plan(&sc, 0, 1) == CFG_PLAN_WRAP,
          "an empty store must open a ring with a snapshot");
    CHECK(cfg_journal_plan(&sc, 0, 0) == CFG_PLAN_REFUSE,
          "with no clean ring there is nowhere to open");
}

static void test_snapshot_and_deltas(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;
    const cfg_device_t *d;

    erase_all();
    put_snapshot(0, 0, 1, 3, 0x0A000001u);
    put_dev(0, CFG_SNAP_SLOTS + 0, 2, 0x200u, CFG_DEV_ENROLLED, 7);
    put_dev(0, CFG_SNAP_SLOTS + 1, 3, 0x101u, CFG_DEV_PAIRED, 1);

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused a good ring");
    CHECK(sc.ring == CFG_RING_A, "wrong ring won");
    CHECK(sc.snap_slot == 0, "snapshot not at slot 0");
    CHECK(sc.deltas == 2, "replayed %u deltas, expected 2", sc.deltas);
    CHECK(sc.next_slot == CFG_SNAP_SLOTS + 2, "next slot is %u", sc.next_slot);
    CHECK(sc.damaged == 0, "%u damaged slots in a clean ring", sc.damaged);
    CHECK(img.cfg.telem_ip == 0x0A000001u, "config head did not survive");
    CHECK(live_devices(&img) == 4, "roster holds %u, expected 4", live_devices(&img));

    d = find_dev(&img, 0x101u);
    CHECK(d != NULL && d->state == CFG_DEV_PAIRED, "the delta did not overwrite by id");
    d = find_dev(&img, 0x200u);
    CHECK(d != NULL && d->slot == 7, "the new device did not take a free entry");
}

/* A removal is a delta in state FREE, and must not spend an entry for nothing. */
static void test_removal(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;

    erase_all();
    put_snapshot(0, 0, 1, 3, 0);
    put_dev(0, CFG_SNAP_SLOTS + 0, 2, 0x101u, CFG_DEV_FREE, 0);
    put_dev(0, CFG_SNAP_SLOTS + 1, 3, 0x999u, CFG_DEV_FREE, 0);

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");
    CHECK(live_devices(&img) == 2, "roster holds %u after one removal, expected 2",
          live_devices(&img));
    CHECK(find_dev(&img, 0x101u) == NULL, "the removed device is still live");
    CHECK(find_dev(&img, 0x999u) == NULL, "a removal invented a device");
    /* find_dev() skips FREE entries, which is what a spent one is left in. */
    CHECK(!id_present(&img, 0x999u),
          "a removal of an unknown device wrote its id into an entry");
    CHECK(untouched_entries(&img) == CFG_DEVICE_MAX - 3u,
          "%u untouched entries, expected %u: a removal spent one",
          untouched_entries(&img), CFG_DEVICE_MAX - 3u);
}

/* The newest snapshot wins wherever it is, and the ring it left is dirty. */
static void test_wrap(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;

    erase_all();
    put_snapshot(0, 0, 1, 2, 0x0A000001u);
    put_snapshot(1, 0, 9, 5, 0x0A000002u);

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");
    CHECK(sc.ring == CFG_RING_B, "the older snapshot won");
    CHECK(sc.snap_seq == 9, "winning seq is %u", sc.snap_seq);
    CHECK(img.cfg.telem_ip == 0x0A000002u, "read the config out of the wrong ring");
    CHECK(live_devices(&img) == 5, "roster came from the wrong ring");
    CHECK(sc.dirty == CFG_RING_A, "the ring left behind was not marked for erasure");
}

/* A torn record is skipped by one slot, and the records after it still resync. */
static void test_torn(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;
    uint8_t *p;

    erase_all();
    put_snapshot(0, 0, 1, 1, 0);
    put_dev(0, CFG_SNAP_SLOTS + 0, 2, 0x300u, CFG_DEV_ENROLLED, 3);
    /* Short by its final flash word: byte-identical to a torn write. */
    p = slot_ptr(0, CFG_SNAP_SLOTS + 1);
    put_dev(0, CFG_SNAP_SLOTS + 1, 3, 0x301u, CFG_DEV_ENROLLED, 4);
    memset(p + CFG_SLOT_BYTES - 32, 0xFF, 32);
    put_dev(0, CFG_SNAP_SLOTS + 2, 4, 0x302u, CFG_DEV_ENROLLED, 5);

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");
    CHECK(find_dev(&img, 0x300u) != NULL, "the record before the tear was lost");
    CHECK(find_dev(&img, 0x301u) == NULL, "a torn record was applied");
    CHECK(find_dev(&img, 0x302u) != NULL, "the scan did not resync past the tear");
    CHECK(sc.next_slot == CFG_SNAP_SLOTS + 3, "next slot is %u", sc.next_slot);
}

/* A resync inside a snapshot must not read its entries as records. */
static void test_entries_are_not_records(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;
    uint16_t inside;
    int seen = 0;

    erase_all();
    put_snapshot(0, 0, 1, CFG_DEVICE_MAX, 0);

    for (inside = 1; inside < CFG_SNAP_SLOTS; inside++) {
        if (cfg_record_valid(slot_ptr(0, inside),
                             (uint16_t)(CFG_SLOTS_PER_SECTOR - inside)))
            seen++;
    }
    CHECK(seen == 0, "%d slots inside a full snapshot parse as records", seen);

    /* Non-vacuous: swapping only the magic must make them parse. */
    {
        cfg_hdr_t *h;
        int now = 0;

        for (inside = 1; inside <= CFG_DEVICE_MAX; inside++) {
            h = slot_ptr(0, (uint16_t)(CFG_SNAP_HEAD_BYTES / CFG_SLOT_BYTES
                                       + inside - 1u));
            h->magic = CFG_MAGIC;
            if (cfg_record_valid(h, (uint16_t)(CFG_SLOTS_PER_SECTOR - inside)))
                now++;
            h->magic = CFG_MAGIC_ENTRY;
        }
        CHECK(now == (int)CFG_DEVICE_MAX,
              "%d of %u entries parse once the magic is a record's; the magic is "
              "not what is separating them", now, CFG_DEVICE_MAX);
    }

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");
    CHECK(sc.deltas == 0, "%u entries inside the snapshot were replayed as deltas",
          sc.deltas);
    CHECK(live_devices(&img) == CFG_DEVICE_MAX, "a full roster did not survive");
}

/* The length is checked first: computing the checksum is what reads off the end. */
static void test_length_is_checked_first(void)
{
    cfg_hdr_t h;

    memset(&h, 0, sizeof(h));
    h.magic   = CFG_MAGIC;
    h.version = CFG_VERSION;
    h.type    = CFG_T_DEV;

    h.slots = 0;
    CHECK(cfg_record_valid(&h, CFG_SLOTS_PER_SECTOR) == 0, "a zero-slot record was accepted");
    h.slots = CFG_SLOTS_PER_SECTOR + 1u;
    CHECK(cfg_record_valid(&h, CFG_SLOTS_PER_SECTOR) == 0,
          "a record longer than the ring was accepted");
    h.slots = 2u;
    CHECK(cfg_record_valid(&h, 1u) == 0, "a record longer than the space left was accepted");
    /* Sealed over the whole claimed length, so only the length can refuse it. */
    {
        static uint8_t big[CFG_SNAP_SLOTS * CFG_SLOT_BYTES];

        memset(big, 0, sizeof(big));
        cfg_record_seal(big, CFG_T_DEV, CFG_SNAP_SLOTS, 1);
        CHECK(cfg_record_crc(big) == ((const cfg_hdr_t *)(const void *)big)->crc,
              "the fixture's own checksum does not hold, so this proves nothing");
        CHECK(cfg_record_valid(big, CFG_SLOTS_PER_SECTOR) == 0,
              "a device record 68 slots long was accepted");

        cfg_record_seal(big, CFG_T_SNAPSHOT, 1, 1);
        CHECK(cfg_record_valid(big, CFG_SLOTS_PER_SECTOR) == 0,
              "a snapshot one slot long was accepted");
    }
}

/* A config change travels as its own one-slot record, and must reach the image. */
static void test_config_delta(void)
{
    cfg_snapshot_t img;
    cfg_config_rec_t c;
    cfg_scan_t sc;

    erase_all();
    put_snapshot(0, 0, 1, 2, 0x0A000001u);

    memset(&c, 0, sizeof(c));
    c.cfg.telem_ip   = 0x0A00007Bu;
    c.cfg.telem_port = 7420;
    c.cfg.ip_static  = 1;
    memcpy(c.cfg.telem_token, "bench-token", sizeof("bench-token"));
    cfg_record_seal(&c, CFG_T_CONFIG, 1, 2);
    memcpy(slot_ptr(0, CFG_SNAP_SLOTS), &c, sizeof(c));

    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");
    CHECK(sc.deltas == 1, "the config delta was not replayed");
    CHECK(img.cfg.telem_ip == 0x0A00007Bu, "the config delta did not reach the image");
    CHECK(img.cfg.telem_port == 7420, "the port did not survive the replay");
    CHECK(strcmp(img.cfg.telem_token, "bench-token") == 0, "the token did not survive");
    CHECK(live_devices(&img) == 2, "a config delta disturbed the roster");
}

/* The plan is arithmetic and is decided before any flash is touched. */
static void test_plan(void)
{
    cfg_scan_t sc;

    memset(&sc, 0, sizeof(sc));
    sc.found = 1;
    sc.next_slot = CFG_SNAP_SLOTS;

    CHECK(cfg_journal_plan(&sc, 0, 1) == CFG_PLAN_DELTA, "a fresh ring must take a delta");
    CHECK(cfg_journal_plan(&sc, CFG_SNAP_EVERY - 1, 1) == CFG_PLAN_DELTA,
          "a checkpoint came due one delta early");
    CHECK(cfg_journal_plan(&sc, CFG_SNAP_EVERY, 1) == CFG_PLAN_SNAPSHOT,
          "a checkpoint did not come due");

    /* Six cycles fit; the seventh checkpoint has nowhere to go and wraps. */
    sc.next_slot = (uint16_t)(6u * CFG_CYCLE_SLOTS);
    CHECK(CFG_SLOTS_PER_SECTOR - sc.next_slot < CFG_SNAP_SLOTS,
          "the fixture does not reach the end of the ring");
    CHECK(cfg_journal_plan(&sc, CFG_SNAP_EVERY, 1) == CFG_PLAN_WRAP,
          "a checkpoint that does not fit must wrap");
    CHECK(cfg_journal_plan(&sc, CFG_SNAP_EVERY, 0) == CFG_PLAN_REFUSE,
          "wrapping onto a dirty ring must be refused, not attempted");
    CHECK(cfg_journal_plan(&sc, 0, 1) == CFG_PLAN_DELTA,
          "a delta still fits where a snapshot does not");

    /* The last slot of the ring: a delta fits, the next one does not. */
    sc.next_slot = CFG_SLOTS_PER_SECTOR - 1u;
    CHECK(cfg_journal_plan(&sc, 0, 1) == CFG_PLAN_DELTA, "the last slot is usable");
    sc.next_slot = CFG_SLOTS_PER_SECTOR;
    CHECK(cfg_journal_plan(&sc, 0, 1) == CFG_PLAN_WRAP, "a full ring must wrap");
}

/* A new runtime parameter must leave snapshots already on flash readable. */
static void test_a_new_config_field_is_readable_over_an_old_snapshot(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;
    const uint8_t *raw;
    uint32_t tail_nonzero = 0;

    erase_all();
    put_snapshot(0, 0, 7, 4, 0x0A00002Au);
    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");

    /* A future field's bytes are pad today, and pad is written as zero. */
    raw = (const uint8_t *)slot_ptr(0, 0);
    for (uint32_t i = sizeof(cfg_hdr_t) + sizeof(cfg_config_t);
         i < CFG_SNAP_HEAD_BYTES; i++) {
        if (raw[i] != 0u)
            tail_nonzero++;
    }
    CHECK(tail_nonzero == 0,
          "%u byte(s) of head padding are not zero, so a new field would read "
          "garbage out of an existing snapshot", tail_nonzero);

    /* And the roster cannot move when the config grows. */
    CHECK(offsetof(cfg_snapshot_t, dev) == CFG_SNAP_HEAD_BYTES,
          "the roster's offset depends on the config's size");
    CHECK(live_devices(&img) == 4, "the roster did not survive");
    CHECK(img.cfg.telem_ip == 0x0A00002Au, "the config did not survive");
}

/* A zero that means "never set" has to be told from a zero that means a value. */
static void test_link_lost_sentinel(void)
{
    cfg_snapshot_t img;
    cfg_scan_t sc;

    erase_all();
    put_snapshot(0, 0, 7, 4, 0x0A00002Au);
    CHECK(cfg_journal_scan(ring[0], ring[1], &img, &sc) == 0, "scan refused");

    /* An existing store carries a zero here, and zero is not a threshold. */
    CHECK(img.cfg.link_lost_misses == 0u, "the field should be unset in this snapshot");
    CHECK(cfg_link_lost_misses(&img.cfg) == CFG_LINK_LOST_DEFAULT,
          "an unset field must resolve to the default, not to zero");

    img.cfg.link_lost_misses = 3u;
    CHECK(cfg_link_lost_misses(&img.cfg) == 3u, "a set field must be used as it stands");
    img.cfg.link_lost_misses = 255u;
    CHECK(cfg_link_lost_misses(&img.cfg) == 255u, "the whole byte must be reachable");
}

/* The geometry the design's erase budget rests on. */
static void test_geometry(void)
{
    uint32_t cycles = CFG_SLOTS_PER_SECTOR / CFG_CYCLE_SLOTS;

    CHECK(sizeof(cfg_device_t) == CFG_SLOT_BYTES, "a device entry is not a slot");
    CHECK(sizeof(cfg_snapshot_t) == CFG_SNAP_SLOTS * CFG_SLOT_BYTES,
          "the snapshot is not a whole number of slots");
    CHECK(cycles == 6, "%u cycles per sector, the design says 6", cycles);
    CHECK(cycles * CFG_SNAP_EVERY == 576,
          "%u changes per erase, the design says 576", cycles * CFG_SNAP_EVERY);
}

int main(void)
{
    test_empty();
    test_snapshot_and_deltas();
    test_removal();
    test_wrap();
    test_torn();
    test_entries_are_not_records();
    test_length_is_checked_first();
    test_config_delta();
    test_a_new_config_field_is_readable_over_an_old_snapshot();
    test_plan();
    test_link_lost_sentinel();
    test_geometry();

    if (failures) {
        printf("cfg: %d FAILURES\n", failures);
        return 1;
    }
    printf("cfg: ok (%u-byte snapshot, %u slots, %u changes per erase)\n",
           (unsigned)sizeof(cfg_snapshot_t), CFG_SNAP_SLOTS,
           (CFG_SLOTS_PER_SECTOR / CFG_CYCLE_SLOTS) * CFG_SNAP_EVERY);
    return 0;
}
