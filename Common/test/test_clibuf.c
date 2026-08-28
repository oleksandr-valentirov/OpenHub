/**
 * @file test_clibuf.c
 * @brief The console's bound, driven past its end where a board could not be made to.
 *
 * Run `2026-08-28-2` lost the `app witness` line off the end of `devices` and
 * nothing said so: the buffer filled, `vsnprintf` reported what it wanted, the
 * caller wrote what fitted and the reader saw a shorter world. What is checked
 * here is that a cut says it was cut, and that an output which fits does not
 * claim one.
 *
 * radio_devices_docs/open_hub/cli.md
 */
#include <stdio.h>
#include <string.h>

#include "cli_buf.h"

static int fails;
static unsigned checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

#define CAP  120

/* The tail is the only place a reader would look for what went missing. */
static int has_note(const char *buf, int len) {
    char tmp[CAP + 1];

    if (len < 0 || len > CAP)
        return 0;
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';
    return strstr(tmp, "-- truncated,") != NULL;
}

int main(void) {
    char buf[CAP];
    uint16_t dropped = 0u;
    int len = 0;
    unsigned i;

    memset(buf, 0, sizeof(buf));

    /* An output that fits loses nothing and is not annotated. */
    len = cli_buf_put(buf, CAP, len, &dropped, "devices 2 of 64\r\n");
    CHECK(len == 17);
    CHECK(dropped == 0u);
    CHECK(cli_buf_note(buf, CAP, len, dropped) == len);
    CHECK(!has_note(buf, len));

    /* One line straddling the end, alone: the loss is the bytes that did not fit. */
    {
        char part[32];
        uint16_t lost = 0u;
        int n = cli_buf_put(part, 32, 0, &lost, "%s", "0123456789012345678901");

        CHECK(n == 22);
        CHECK(lost == 0u);
        /* Fifteen wanted into nine writable bytes, so six are gone. */
        n = cli_buf_put(part, 32, n, &lost, "%s", "abcdefghijklmno");
        CHECK(n == 31);
        CHECK(lost == 6u);
        /* And the next one is lost whole, by the other branch. */
        n = cli_buf_put(part, 32, n, &lost, "%s", "pqrst");
        CHECK(n == 31);
        CHECK(lost == 11u);
    }

    /* One line straddling the end: the count is the bytes, not the lines. */
    for (i = 0; i < 20u; i++)
        len = cli_buf_put(buf, CAP, len, &dropped, "%02u: six-and-thirty bytes of device\r\n", i);
    CHECK(len == CAP - 1);
    CHECK(dropped > 0u);

    /* A caller that keeps writing after the end still has its loss counted. */
    {
        uint16_t before = dropped;

        len = cli_buf_put(buf, CAP, len, &dropped, "app witness 3 agreed, 0 disagreed\r\n");
        CHECK(len == CAP - 1);
        CHECK(dropped == (uint16_t)(before + 35u));
    }

    len = cli_buf_note(buf, CAP, len, dropped);
    CHECK(len <= CAP);
    CHECK(has_note(buf, len));

    /* The note overwrites the tail rather than shortening every command. */
    CHECK(len >= CAP - 1);

    /* Saturation is a reading too, and it says which one it is. */
    {
        char big[CAP];
        uint16_t sat = 65535u;
        int n = cli_buf_note(big, CAP, 0, sat);

        CHECK(n > 0);
        CHECK(memchr(big, '+', (size_t)n) != NULL);
    }

    /* An empty population: nothing lost, nothing written, no note. */
    {
        char empty[CAP];
        uint16_t none = 0u;

        memset(empty, 0x7Fu, sizeof(empty));
        CHECK(cli_buf_note(empty, CAP, 0, none) == 0);
        CHECK(empty[0] == 0x7F);
        CHECK(cli_buf_put(empty, CAP, 0, &none, "%s", "") == 0);
        CHECK(none == 0u);
    }

    /* A buffer with no room for the note is left as it was, not corrupted. */
    {
        char tiny[8];
        uint16_t some = 400u;

        memset(tiny, 'x', sizeof(tiny));
        CHECK(cli_buf_note(tiny, (int)sizeof(tiny), 4, some) == 4);
        CHECK(tiny[4] == 'x');
    }

    if (fails) {
        printf("\n%d of %u clibuf check(s) failed\n", fails, checks);
        return 1;
    }
    printf("clibuf: ok (%u checks)\n", checks);
    return 0;
}
