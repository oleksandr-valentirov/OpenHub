/**
 * @file test_oht.c
 * @brief This codec against the server's, over vectors neither side produced alone.
 *
 * radio_devices_docs/open_hub/network/telemetry.md
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "oht_proto.h"
#include "oht_v1.h"

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* Prints the first differing byte: "lengths differ" names no offset to look at. */
static void expect_bytes(const char *name, const oht_writer_t *w,
                         const unsigned char *want, size_t want_len) {
    if (w->err) {
        printf("FAIL %s: the writer latched an error\n", name);
        fails++;
        return;
    }
    if (w->len != want_len) {
        printf("FAIL %s: wrote %u bytes, vector is %u\n",
               name, (unsigned)w->len, (unsigned)want_len);
        fails++;
        return;
    }
    for (size_t i = 0; i < want_len; i++) {
        if (w->buf[i] != want[i]) {
            printf("FAIL %s: byte %u is 0x%02X, vector says 0x%02X\n",
                   name, (unsigned)i, w->buf[i], want[i]);
            fails++;
            return;
        }
    }
}

static int64_t read_field(const unsigned char *body, size_t len, uint16_t id) {
    oht_field_t f;
    int64_t v = 0;

    if (oht_find(body, (uint16_t)len, id, &f) != 1) {
        printf("FAIL field 0x%04X is not in the body\n", id);
        fails++;
        return 0;
    }
    if (oht_field_int(&f, &v) != 0) {
        printf("FAIL field 0x%04X did not read as an integer\n", id);
        fails++;
    }
    return v;
}

static void test_widths(void) {
    uint8_t buf[256];
    oht_writer_t w;

    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_DEVICE, 0xC4D444AAu) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_SLOT, 7) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_RSSI_UP_SYNC_DBM, -43) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_SUPPLY_MV, 3287) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_TEMP_C_X10, -173) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_AFC_HZ, -12207) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_FRAMES_OK, 4000000000u) == 0);
    CHECK(OHT_PUT(&w, OHT_F_DEVICE_LAST_SUPERFRAME, 0x7FFFFFFF) == 0);
    expect_bytes("widths", &w, oht_v1_widths, sizeof(oht_v1_widths));
}

static void test_multi(void) {
    uint8_t buf[256];
    oht_writer_t w;

    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_HUB, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_F_HUB_SUPERFRAME, 12345) == 0);
    CHECK(OHT_PUT(&w, OHT_F_HUB_CALIB_PPM, -37) == 0);
    CHECK(OHT_PUT(&w, OHT_F_HUB_IPC_READY, 1) == 0);
    CHECK(oht_rec_begin(&w, OHT_OBJ_RXDIAG, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_F_RXDIAG_UP_RSSI_FLOOR_DBM, -96) == 0);
    CHECK(OHT_PUT(&w, OHT_F_RXDIAG_SYNC_MATCH, 21) == 0);
    CHECK(oht_rec_begin(&w, OHT_OBJ_FRAME, 0xC4D444AAu) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_GRID, 3) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_RSSI_DBM, -43) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_LNA_GAIN, OHT_LNA_GAIN_G1_MAX) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_CRC_OK, 1) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_IN_FRAME, 0) == 0);
    expect_bytes("multi", &w, oht_v1_multi, sizeof(oht_v1_multi));
}

static void test_empty_rec(void) {
    uint8_t buf[64];
    oht_writer_t w;

    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_LINK, 0) == 0);
    CHECK(oht_rec_begin(&w, OHT_OBJ_LINK, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_F_LINK_CONNECTS, 1) == 0);
    expect_bytes("empty_rec", &w, oht_v1_empty_rec, sizeof(oht_v1_empty_rec));
}

static void test_cmd_args(void) {
    uint8_t buf[64];
    oht_writer_t w;
    oht_field_t f;

    /* Command arguments are bare TLVs, so the record opened here is stripped. */
    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_HUB, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_A_DEV_ID, 0xC4D444AAu) == 0);
    CHECK(OHT_PUT(&w, OHT_A_RATE, 12) == 0);
    CHECK(OHT_PUT(&w, OHT_A_REPEATS, 4) == 0);
    CHECK(w.len > OHT_REC_HDR_LEN);
    CHECK(w.len - OHT_REC_HDR_LEN == sizeof(oht_v1_cmd_args));
    CHECK(memcmp(buf + OHT_REC_HDR_LEN, oht_v1_cmd_args,
                 sizeof(oht_v1_cmd_args)) == 0);

    /* And the same bytes read back, which is the direction the hub uses. */
    CHECK(read_field(oht_v1_cmd_args, sizeof(oht_v1_cmd_args), 0x8000u)
          == (int64_t)0xC4D444AAu);
    CHECK(read_field(oht_v1_cmd_args, sizeof(oht_v1_cmd_args), 0x8001u) == 12);
    CHECK(oht_find(oht_v1_cmd_args, sizeof(oht_v1_cmd_args), 0x8007u, &f) == 0);
}

static void test_blobs(void) {
    uint8_t buf[128];
    uint8_t key[33];
    oht_writer_t w;

    for (unsigned i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;

    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_HUB, 0) == 0);
    CHECK(OHT_PUT_BYTES(&w, OHT_A_PUBKEY, key, sizeof(key)) == 0);
    CHECK(OHT_PUT_BYTES(&w, OHT_A_APP, key, 0) == 0);
    CHECK(oht_put_bytes(&w, 0x7FFFu, OHT_T_STR, "openhub", 7) == 0);
    expect_bytes("blobs", &w, oht_v1_blobs, sizeof(oht_v1_blobs));
}

/* Nothing above this line drives the paths that only run when something is wrong. */
static void test_refusals(void) {
    uint8_t small[10];
    uint8_t buf[64];
    oht_writer_t w;
    oht_reader_t r;
    oht_field_t f;
    int64_t v;

    /* A field with no record open is refused, not silently made into one. */
    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(OHT_PUT(&w, OHT_F_HUB_SUPERFRAME, 1) == -1);
    CHECK(OHT_FAILED(&w));

    /* The latch is sticky: a later call that would fit still refuses. */
    CHECK(oht_rec_begin(&w, OHT_OBJ_HUB, 0) == -1);

    /* Overflow is refused at the field that would not fit, and latched. */
    oht_writer_init(&w, small, sizeof(small));
    CHECK(oht_rec_begin(&w, OHT_OBJ_HUB, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_F_HUB_SUPERFRAME, 1) == -1);
    CHECK(OHT_FAILED(&w));
    CHECK(w.len == OHT_REC_HDR_LEN);

    /* A truncated field header is a decode failure, never an early clean end. */
    oht_reader_init(&r, "\x01\x02\x03", 3u);
    CHECK(oht_next(&r, &f) == -1);

    /* A length that runs past the buffer is refused rather than clamped. */
    oht_reader_init(&r, "\x00\x01\x05\x04\x01\x02", 6u);
    CHECK(oht_next(&r, &f) == -1);

    /* A u32 field that arrived four bytes short is not the number zero. */
    oht_reader_init(&r, "\x00\x01\x05\x01\x07", 5u);
    CHECK(oht_next(&r, &f) == 1);
    CHECK(oht_field_int(&f, &v) == -1);

    /* Bytes are not integers; asking for one must fail rather than reinterpret. */
    oht_reader_init(&r, "\x00\x01\x0A\x02\x01\x02", 6u);
    CHECK(oht_next(&r, &f) == 1);
    CHECK(oht_field_int(&f, &v) == -1);

    /* An unknown type code decodes as far as its length and no further. */
    oht_reader_init(&r, "\x34\x12\x63\x02\xAA\xBB", 6u);
    CHECK(oht_next(&r, &f) == 1);
    CHECK(f.id == 0x1234u && f.len == 2u);
    CHECK(oht_field_int(&f, &v) == -1);
    CHECK(oht_next(&r, &f) == 0);

    /* A bool normalises on the way out: the wire never carries a truthy 2. */
    oht_writer_init(&w, buf, sizeof(buf));
    CHECK(oht_rec_begin(&w, OHT_OBJ_FRAME, 0) == 0);
    CHECK(OHT_PUT(&w, OHT_F_FRAME_CRC_OK, 2) == 0);
    CHECK(buf[OHT_REC_HDR_LEN + OHT_FLD_HDR_LEN] == 1u);
}

int main(void) {
    /* The vectors and the field table have to come from the same schema run. */
    CHECK(strcmp(OHT_VECTORS_SCHEMA, OHT_SCHEMA_DIGEST) == 0);

    test_widths();
    test_multi();
    test_empty_rec();
    test_cmd_args();
    test_blobs();
    test_refusals();

    if (fails == 0)
        printf("oht: ok (schema %s, vectors %s)\n",
               OHT_SCHEMA_DIGEST, OHT_VECTORS_DIGEST);
    return fails != 0;
}
