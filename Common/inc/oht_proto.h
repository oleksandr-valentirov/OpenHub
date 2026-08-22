#pragma once

#include <stdint.h>
#include <stddef.h>

#include "oht_fields.h"

/**
 * @file oht_proto.h
 * @brief The northbound telemetry wire: frame headers, records, and the TLV writer.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */

/* 'OHT1' read as a little-endian word; the whole protocol is little-endian. */
#define OHT_MAGIC          0x3154484FuL
#define OHT_VERSION        1u
/* The transmit buffer, and therefore the real cap the server is told about. */
#define OHT_MAX_PAYLOAD    4096u

/* Message types. Hub to server unless the comment says otherwise. */
enum {
    OHT_MSG_HELLO     = 0x01,
    OHT_MSG_HELLO_ACK = 0x02,  /**< server to hub */
    OHT_MSG_SNAPSHOT  = 0x10,  /**< every object the hub knows, on a timer */
    OHT_MSG_DELTA     = 0x11,  /**< only what changed since the last snapshot */
    OHT_MSG_EVENT     = 0x12,  /**< pushed when it happened, never on a timer */
    OHT_MSG_CMD       = 0x20,  /**< server to hub */
    OHT_MSG_CMD_ACK   = 0x21,
    OHT_MSG_PING      = 0x30,
    OHT_MSG_PONG      = 0x31,
    OHT_MSG_LOG       = 0x7F
};

/* An EVENT's cause rides the frame flags, so a reader routes it undecoded. */
enum {
    OHT_EVT_UPLINK        = 1,
    OHT_EVT_PAIRED        = 2,
    OHT_EVT_CMD_DELIVERED = 3,  /**< the device echoed the sequence number back */
    OHT_EVT_CMD_LOST      = 4,  /**< every repeat spent and no echo came */
    OHT_EVT_PAIR_WINDOW   = 5,
    OHT_EVT_ERROR         = 6
};

/* Why HELLO was refused, so the console says it without reading a server log. */
enum {
    OHT_ACK_OK              = 0,
    OHT_ACK_BAD_TOKEN       = 1,
    OHT_ACK_BAD_VERSION     = 2,
    OHT_ACK_SCHEMA_MISMATCH = 3,
    OHT_ACK_BUSY            = 4
};

/* Type codes. Permanent, exactly like the field ids in oht_fields.h. */
enum {
    OHT_T_U8    = 1,
    OHT_T_I8    = 2,
    OHT_T_U16   = 3,
    OHT_T_I16   = 4,
    OHT_T_U32   = 5,
    OHT_T_I32   = 6,
    OHT_T_U64   = 7,
    OHT_T_I64   = 8,
    OHT_T_BOOL  = 9,
    OHT_T_BYTES = 10,
    OHT_T_STR   = 11
};

/** @brief Every frame's first eight bytes. */
typedef struct oht_hdr {
    uint8_t  ver;
    uint8_t  type;
    uint16_t len;     /**< payload bytes following this header */
    uint16_t seq;     /**< per direction; a CMD_ACK echoes the CMD's */
    uint16_t flags;   /**< an OHT_EVT_* on an event, otherwise zero */
} __attribute__((packed)) oht_hdr_t;

#define OHT_HDR_LEN  8u

/**
 * @brief The first message, a fixed struct so a schema mismatch still parses.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */
typedef struct oht_hello {
    uint32_t magic;
    uint8_t  proto_ver;
    uint8_t  reserved[3];
    uint32_t hub_id;
    uint32_t boot_id;      /**< redrawn each boot: a reset is not a reconnect */
    uint32_t uptime_ms;
    char     fw[24];
    char     schema_digest[17];
    char     pair_digest[17];
    char     hop_digest[17];
    uint8_t  reserved2[3];
    uint8_t  token[32];    /**< all zero when no shared secret is configured */
} __attribute__((packed)) oht_hello_t;

/** @brief The server's answer, carrying the cadence it wants and its own digest. */
typedef struct oht_hello_ack {
    uint8_t  accepted;
    uint8_t  reason;       /**< OHT_ACK_* */
    uint8_t  reserved[2];
    uint32_t server_time_s;
    uint16_t snapshot_ms;
    uint16_t max_payload;
    char     schema_digest[17];
    uint8_t  reserved2[3];
} __attribute__((packed)) oht_hello_ack_t;

/** @brief A record's header; the fields follow it packed, `nfields` of them. */
typedef struct oht_rec_hdr {
    uint8_t  obj;          /**< OHT_OBJ_* */
    uint8_t  nfields;
    uint16_t reserved;
    uint32_t key;          /**< a dev_id, or zero when the object has no key */
} __attribute__((packed)) oht_rec_hdr_t;

#define OHT_REC_HDR_LEN  8u
#define OHT_FLD_HDR_LEN  4u

/** @brief What a CMD asks for; TLV arguments follow. */
typedef struct oht_cmd_hdr {
    uint16_t cmd_id;
    uint16_t reserved;
    uint32_t target;       /**< a dev_id for device-scoped commands, else zero */
} __attribute__((packed)) oht_cmd_hdr_t;

/** @brief The verdict. `queued` is not `ok`: nothing has reached the air yet. */
typedef struct oht_cmd_ack {
    uint16_t cmd_seq;      /**< off the CMD frame's own header */
    uint8_t  result;       /**< OHT_RES_* */
    uint8_t  detail;       /**< the underlying IPC status, when there was one */
    uint8_t  reserved[4];
} __attribute__((packed)) oht_cmd_ack_t;

_Static_assert(sizeof(oht_hdr_t)     == OHT_HDR_LEN,     "the frame header is 8 bytes");
_Static_assert(sizeof(oht_rec_hdr_t) == OHT_REC_HDR_LEN, "the record header is 8 bytes");
_Static_assert(sizeof(oht_cmd_hdr_t) == 8u,              "the command header is 8 bytes");
_Static_assert(sizeof(oht_cmd_ack_t) == 8u,              "the command ack is 8 bytes");
_Static_assert(sizeof(oht_hello_t)   == 130u,            "hello is 130 bytes");
_Static_assert(sizeof(oht_hello_ack_t) == 32u,           "hello_ack is 32 bytes");

/**
 * @brief A body under construction: one buffer, one open record, and an error latch.
 *
 * `err` latches rather than each call returning: a builder that checks nothing
 * until the end cannot write a truncated record and call it a short one.
 * radio_devices_docs/open_hub/network/telemetry.md
 */
typedef struct oht_writer {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;
    int16_t  rec_at;    /**< offset of the open record's header, -1 when none */
    uint8_t  err;       /**< sticky: set once, never cleared by a later success */
} oht_writer_t;

/**
 * @brief Points a writer at a caller's buffer and empties it.
 * @param w    the writer
 * @param buf  where the body is built
 * @param cap  its size
 */
void oht_writer_init(oht_writer_t *w, uint8_t *buf, uint16_t cap);

/**
 * @brief Opens a record, closing any that was open.
 * @param w    the writer
 * @param obj  OHT_OBJ_*
 * @param key  the object's key, or zero when it has none
 * @retval 0  opened
 * @retval -1 the buffer is full; the error latch is set
 */
int oht_rec_begin(oht_writer_t *w, uint8_t obj, uint32_t key);

/**
 * @brief Appends one integer or boolean field to the open record.
 * @param w     the writer
 * @param id    the field id
 * @param type  its type code, which decides the width written
 * @param v     the value, narrowed to that width
 * @retval 0  appended
 * @retval -1 no record is open, the type is not integral, or the buffer is full
 *
 * Callers pass id and type as one OHT_F_* macro, so the two cannot be mismatched.
 */
int oht_put(oht_writer_t *w, uint16_t id, uint8_t type, int64_t v);

/**
 * @brief Appends one bytes or string field.
 * @param w     the writer
 * @param id    the field id
 * @param type  OHT_T_BYTES or OHT_T_STR
 * @param p     the value
 * @param len   its length, at most 255
 * @retval 0  appended
 * @retval -1 no record is open, the type is wrong, or it does not fit
 */
int oht_put_bytes(oht_writer_t *w, uint16_t id, uint8_t type,
                  const void *p, uint8_t len);

/** @brief Whether anything was refused since oht_writer_init(). */
#define OHT_FAILED(w)  ((w)->err != 0u)

/* Two tokens per field, so no call can pair one field's id with another's width. */
#define OHT_PUT(w, field, v)          oht_put((w), field, (int64_t)(v))
#define OHT_PUT_BYTES(w, field, p, n) oht_put_bytes((w), field, (p), (n))

/**
 * @brief One decoded field, as the reader hands it back.
 *
 * `raw` points into the caller's buffer; nothing is copied.
 */
typedef struct oht_field {
    uint16_t       id;
    uint8_t        type;
    uint8_t        len;
    const uint8_t *raw;
} oht_field_t;

/**
 * @brief Walks the TLVs of one command or record body.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */
typedef struct oht_reader {
    const uint8_t *buf;
    uint16_t       len;
    uint16_t       off;
} oht_reader_t;

/**
 * @brief Points a reader at a body.
 * @param r    the reader
 * @param buf  the bytes
 * @param len  how many
 */
void oht_reader_init(oht_reader_t *r, const void *buf, uint16_t len);

/**
 * @brief Takes the next field.
 * @param r  the reader
 * @param f  receives it, pointing into the reader's buffer
 * @retval 1  a field was taken
 * @retval 0  the body ended cleanly
 * @retval -1 a length ran past the end; nothing after it is read
 */
int oht_next(oht_reader_t *r, oht_field_t *f);

/**
 * @brief Reads an integral field as a signed 64-bit value.
 * @param f    the field
 * @param out  receives the value, sign-extended for the signed types
 * @retval 0  read
 * @retval -1 the length does not match the type, or the type is not integral
 *
 * A width mismatch is refused rather than padded: a field that arrived short is
 * not a small number.
 */
int oht_field_int(const oht_field_t *f, int64_t *out);

/**
 * @brief Finds one field id in a body, for a command's arguments.
 * @param buf  the body
 * @param len  its length
 * @param id   the argument id
 * @param f    receives it
 * @retval 1  found
 * @retval 0  absent
 * @retval -1 the body is malformed
 */
int oht_find(const void *buf, uint16_t len, uint16_t id, oht_field_t *f);
