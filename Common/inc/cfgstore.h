/**
 * @file cfgstore.h
 * @brief The hub's configuration store on flash: a ring of checkpoints.
 *
 * ADR-0027. Fixed-size typed records wrapping between two sectors, with periodic
 * self-contained snapshots and small deltas between them.
 * radio_devices_docs/open_hub/arch/config-store.md
 */
#ifndef CFGSTORE_H
#define CFGSTORE_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_layout.h"

#define CFG_MAGIC            0x4F484347u   /* "OHCG": a record starting at a slot */
#define CFG_VERSION          1u

/* Entries carry their own magic, or a resync past a tear reads them as records.
 * radio_devices_docs/open_hub/arch/config-store.md */
#define CFG_MAGIC_ENTRY      0x4F484345u   /* "OHCE": an entry inside a snapshot */

/* Fixed: at a fixed stride a torn record is skipped, not lost track of.
 * radio_devices_docs/open_hub/arch/config-store.md */
#define CFG_SLOT_BYTES       128u
#define CFG_SECTOR_BYTES     0x20000u
#define CFG_SLOTS_PER_SECTOR (CFG_SECTOR_BYTES / CFG_SLOT_BYTES)

#define CFG_DEVICE_MAX       64u
#define CFG_SNAP_HEAD_BYTES  512u
#define CFG_SNAP_BYTES       (CFG_SNAP_HEAD_BYTES + CFG_DEVICE_MAX * CFG_SLOT_BYTES)
#define CFG_SNAP_SLOTS       (CFG_SNAP_BYTES / CFG_SLOT_BYTES)

/* Deltas between checkpoints. Six cycles fit a sector, so one erase per 576
 * changes. radio_devices_docs/open_hub/arch/config-store.md */
#define CFG_SNAP_EVERY       96u
#define CFG_CYCLE_SLOTS      (CFG_SNAP_SLOTS + CFG_SNAP_EVERY)

#define CFG_PUBKEY_BYTES     32u
#define CFG_ROOT_KEY_BYTES   32u
#define CFG_SESSION_BYTES    16u
#define CFG_NONCE_BYTES      8u
#define CFG_TOKEN_BYTES      33u   /**< the northbound token, NUL-terminated */

/* Zero is "never set", not "lost after none": an older snapshot's pad reads as
 * zero. radio_devices_docs/open_hub/arch/config-store.md */
#define CFG_LINK_LOST_DEFAULT  12u

/* Bank 1. The identity is apart from the ring because the ring's sectors get
 * erased. radio_devices_docs/open_hub/arch/config-store.md */
#define CFG_IDENTITY_SECTOR  5u
#define CFG_JOURNAL_SECTOR_A 6u
#define CFG_JOURNAL_SECTOR_B 7u
#define CFG_IDENTITY_ADDR    0x080A0000u
#define CFG_JOURNAL_ADDR_A   0x080C0000u
#define CFG_JOURNAL_ADDR_B   0x080E0000u

/** Record types. A slot whose magic does not match is free or damaged. */
enum {
    CFG_T_FREE     = 0,
    CFG_T_SNAPSHOT = 1,   /**< the whole config and roster, self-contained */
    CFG_T_DEV      = 2,   /**< one device entry that changed */
    CFG_T_CONFIG   = 3,   /**< the config head alone, with no roster */
    CFG_T_IDENTITY = 4    /**< sector 5 only: the hub scalar and the network key */
};

/** How far a device has got. A removal is an entry in state FREE, not a hole. */
enum {
    CFG_DEV_FREE = 0,
    CFG_DEV_ENROLLED,   /**< the operator supplied an id; no key yet - ADR-0024 */
    CFG_DEV_PAIRED      /**< the exchange completed and a root key is present */
};

/** @brief The first sixteen bytes of every record, and of every snapshot entry. */
typedef struct cfg_hdr {
    uint32_t magic;
    uint32_t crc;      /**< over the bytes after this field, for slots*128 - 8 */
    uint8_t  version;
    uint8_t  type;     /**< CFG_T_* */
    uint16_t slots;    /**< 128-byte slots this record occupies */
    uint32_t seq;      /**< monotonic across both sectors; the newest wins */
} __attribute__((packed)) cfg_hdr_t;

/** @brief One device: exactly one slot, and the same shape inside a snapshot. */
typedef struct cfg_device {
    cfg_hdr_t hdr;
    uint32_t  dev_id;
    uint32_t  key_gen;                        /**< the generation rx_floor belongs to */
    uint32_t  rotate_epoch;                   /**< superframe / SUPERFRAME_PER_DAY at pairing */
    uint32_t  rx_floor;                       /**< scoped to key_gen, never to a device */
    uint32_t  tx_floor;                       /**< carried forward across pairings */
    uint8_t   state;                          /**< CFG_DEV_* */
    uint8_t   slot;                           /**< uplink slot, assigned at enrolment */
    uint8_t   spare[2];                       /**< named padding, so a field can be added */
    uint8_t   pubkey[CFG_PUBKEY_BYTES];       /**< zero until PAIR_REQ brings it */
    uint8_t   root_key[CFG_ROOT_KEY_BYTES];   /**< HKDF root; zero until pairing lands */
    uint8_t   session_key[CFG_SESSION_BYTES]; /**< stored, not re-derived: Z is not kept */
    uint8_t   last_nonce[CFG_NONCE_BYTES];    /**< the last accepted; a repeat is refused */
} __attribute__((packed)) cfg_device_t;

/** @brief Everything an operator sets that must survive a reset. */
typedef struct cfg_config {
    uint32_t telem_ip;                    /**< the server the hub dials out to */
    uint16_t telem_port;
    uint8_t  ip_static;                   /**< 0 leaves the address to DHCP */
    uint8_t  report_every;                /**< what a future pairing grants, in superframes */
    uint32_t ip_addr;
    uint32_t ip_mask;
    uint32_t ip_gw;
    char     telem_token[CFG_TOKEN_BYTES];
    uint8_t  link_lost_misses;            /**< missed reports before the link counts as lost */
    uint8_t  spare[2];
} __attribute__((packed)) cfg_config_t;

/** @brief The configured value, or the default a snapshot that predates it gives. */
static inline uint8_t cfg_link_lost_misses(const cfg_config_t *c) {
    return (c->link_lost_misses == 0u) ? CFG_LINK_LOST_DEFAULT : c->link_lost_misses;
}

/** @brief One config change on its own: one slot, and the head a snapshot carries. */
typedef struct cfg_config_rec {
    cfg_hdr_t    hdr;
    cfg_config_t cfg;
    uint8_t      pad[CFG_SLOT_BYTES - sizeof(cfg_hdr_t) - sizeof(cfg_config_t)];
} __attribute__((packed)) cfg_config_rec_t;

/** @brief A checkpoint: nothing older than the newest valid one is ever read. */
typedef struct cfg_snapshot {
    cfg_hdr_t    hdr;
    cfg_config_t cfg;
    uint8_t      pad[CFG_SNAP_HEAD_BYTES - sizeof(cfg_hdr_t) - sizeof(cfg_config_t)];
    cfg_device_t dev[CFG_DEVICE_MAX];
} __attribute__((packed)) cfg_snapshot_t;

/** @brief Sector 5: written once at provisioning, never erased in normal operation. */
typedef struct cfg_identity {
    cfg_hdr_t hdr;
    uint8_t   hub_priv[CFG_ROOT_KEY_BYTES];   /**< X25519 scalar - ADR-0025 */
    uint8_t   net_key[CFG_SESSION_BYTES];     /**< the hop key every device shares */
    uint8_t   spare[64];
} __attribute__((packed)) cfg_identity_t;

_Static_assert(sizeof(cfg_hdr_t) == 16, "a record header is half a flash word");
_Static_assert(sizeof(cfg_device_t) == 128, "a device entry is four flash words");
_Static_assert(sizeof(cfg_identity_t) <= CFG_SLOT_BYTES, "the identity is one slot");
_Static_assert(sizeof(cfg_config_rec_t) == CFG_SLOT_BYTES, "a config record is one slot");
_Static_assert(sizeof(cfg_snapshot_t) % 32 == 0, "a snapshot is whole flash words");
_Static_assert(sizeof(cfg_snapshot_t) % CFG_SLOT_BYTES == 0, "a snapshot is whole slots");
_Static_assert(sizeof(cfg_snapshot_t) == CFG_SNAP_BYTES, "the snapshot is the size claimed");
/* Why a new config field is cheap: the roster's offset is the head's size.
 * radio_devices_docs/open_hub/arch/config-store.md */
_Static_assert(offsetof(cfg_snapshot_t, dev) == CFG_SNAP_HEAD_BYTES,
               "the roster's offset must not depend on the config's size");

_Static_assert(CFG_DEVICE_MAX == 64, "the roster is the grid's device cap");
_Static_assert(CFG_DEVICE_MAX >= RADIO_DEVICE_MAX, "the store is smaller than the grid");
_Static_assert(CFG_SNAP_SLOTS + CFG_SNAP_EVERY <= CFG_SLOTS_PER_SECTOR,
               "a cycle must fit the sector it wraps inside");
/* The design's whole point as a compile-time check, and it may never be relaxed.
 * radio_devices_docs/open_hub/arch/config-store.md */
_Static_assert(CFG_IDENTITY_SECTOR != CFG_JOURNAL_SECTOR_A &&
               CFG_IDENTITY_SECTOR != CFG_JOURNAL_SECTOR_B,
               "the identity must not be erased when the ring wraps");

#endif /* CFGSTORE_H */
