/**
 * @file oht_codec.c
 * @brief The TLV writer and reader, built for both cores and for the host tests.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */
#include <string.h>

#include "oht_proto.h"

/* Widths are the wire's, not the host's: nothing here writes a native integer. */
static uint8_t type_width(uint8_t type) {
    switch (type) {
    case OHT_T_U8:
    case OHT_T_I8:
    case OHT_T_BOOL: return 1u;
    case OHT_T_U16:
    case OHT_T_I16:  return 2u;
    case OHT_T_U32:
    case OHT_T_I32:  return 4u;
    case OHT_T_U64:
    case OHT_T_I64:  return 8u;
    default:         return 0u;
    }
}

static void put_le(uint8_t *p, uint64_t v, uint8_t width) {
    for (uint8_t i = 0; i < width; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}

static uint64_t get_le(const uint8_t *p, uint8_t width) {
    uint64_t v = 0;

    for (uint8_t i = 0; i < width; i++)
        v |= (uint64_t)p[i] << (8u * i);
    return v;
}

void oht_writer_init(oht_writer_t *w, uint8_t *buf, uint16_t cap) {
    w->buf    = buf;
    w->cap    = cap;
    w->len    = 0;
    w->rec_at = -1;
    w->err    = 0;
}

int oht_rec_begin(oht_writer_t *w, uint8_t obj, uint32_t key) {
    oht_rec_hdr_t h;

    if (w->err)
        return -1;
    if ((uint32_t)w->len + OHT_REC_HDR_LEN > w->cap) {
        w->err = 1;
        return -1;
    }
    h.obj      = obj;
    h.nfields  = 0;
    h.reserved = 0;
    h.key      = key;
    memcpy(&w->buf[w->len], &h, sizeof(h));
    w->rec_at = (int16_t)w->len;
    w->len    = (uint16_t)(w->len + OHT_REC_HDR_LEN);
    return 0;
}

/* The count lives in the header, so every append has to reach back and bump it. */
static int bump_count(oht_writer_t *w) {
    uint8_t *n = &w->buf[(uint16_t)w->rec_at + 1u];

    if (*n == 255u) {
        w->err = 1;
        return -1;
    }
    (*n)++;
    return 0;
}

int oht_put(oht_writer_t *w, uint16_t id, uint8_t type, int64_t v) {
    uint8_t width = type_width(type);
    uint16_t need;

    if (w->err || w->rec_at < 0 || width == 0u) {
        w->err = 1;
        return -1;
    }
    need = (uint16_t)(OHT_FLD_HDR_LEN + width);
    if ((uint32_t)w->len + need > w->cap) {
        w->err = 1;
        return -1;
    }
    if (bump_count(w) != 0)
        return -1;

    w->buf[w->len]     = (uint8_t)id;
    w->buf[w->len + 1] = (uint8_t)(id >> 8);
    w->buf[w->len + 2] = type;
    w->buf[w->len + 3] = width;
    /* A bool is normalised here: the wire carries 0 or 1, never a truthy 2. */
    put_le(&w->buf[w->len + 4], (type == OHT_T_BOOL) ? (v != 0) : (uint64_t)v, width);
    w->len = (uint16_t)(w->len + need);
    return 0;
}

int oht_put_bytes(oht_writer_t *w, uint16_t id, uint8_t type,
                  const void *p, uint8_t len) {
    uint16_t need;

    if (w->err || w->rec_at < 0 ||
        (type != OHT_T_BYTES && type != OHT_T_STR) || (p == NULL && len != 0u)) {
        w->err = 1;
        return -1;
    }
    need = (uint16_t)(OHT_FLD_HDR_LEN + len);
    if ((uint32_t)w->len + need > w->cap) {
        w->err = 1;
        return -1;
    }
    if (bump_count(w) != 0)
        return -1;

    w->buf[w->len]     = (uint8_t)id;
    w->buf[w->len + 1] = (uint8_t)(id >> 8);
    w->buf[w->len + 2] = type;
    w->buf[w->len + 3] = len;
    if (len != 0u)
        memcpy(&w->buf[w->len + 4], p, len);
    w->len = (uint16_t)(w->len + need);
    return 0;
}

void oht_reader_init(oht_reader_t *r, const void *buf, uint16_t len) {
    r->buf = (const uint8_t *)buf;
    r->len = len;
    r->off = 0;
}

int oht_next(oht_reader_t *r, oht_field_t *f) {
    uint16_t left = (uint16_t)(r->len - r->off);

    if (r->off >= r->len)
        return 0;
    if (left < OHT_FLD_HDR_LEN)
        return -1;

    f->id   = (uint16_t)(r->buf[r->off] | ((uint16_t)r->buf[r->off + 1] << 8));
    f->type = r->buf[r->off + 2];
    f->len  = r->buf[r->off + 3];
    if ((uint16_t)(left - OHT_FLD_HDR_LEN) < f->len)
        return -1;
    f->raw = &r->buf[r->off + OHT_FLD_HDR_LEN];
    r->off = (uint16_t)(r->off + OHT_FLD_HDR_LEN + f->len);
    return 1;
}

int oht_field_int(const oht_field_t *f, int64_t *out) {
    uint8_t width = type_width(f->type);
    uint64_t raw;

    if (width == 0u || f->len != width)
        return -1;
    raw = get_le(f->raw, width);
    switch (f->type) {
    case OHT_T_I8:  *out = (int8_t)raw;  break;
    case OHT_T_I16: *out = (int16_t)raw; break;
    case OHT_T_I32: *out = (int32_t)raw; break;
    case OHT_T_I64: *out = (int64_t)raw; break;
    default:        *out = (int64_t)raw; break;
    }
    return 0;
}

int oht_find(const void *buf, uint16_t len, uint16_t id, oht_field_t *f) {
    oht_reader_t r;
    int rc;

    oht_reader_init(&r, buf, len);
    while ((rc = oht_next(&r, f)) == 1) {
        if (f->id == id)
            return 1;
    }
    return rc;
}
