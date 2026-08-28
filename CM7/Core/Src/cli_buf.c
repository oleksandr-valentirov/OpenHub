/* The console's bound, where it can be driven without a board.
 * radio_devices_docs/open_hub/cli.md */
#include "cli_buf.h"

#include <stdio.h>
#include <string.h>

/* A lost byte the counter cannot hold is still a lost byte: saturate, say so. */
static void bump(uint16_t *dropped, int n) {
    uint32_t sum;

    if (dropped == NULL || n <= 0)
        return;
    sum = (uint32_t)*dropped + (uint32_t)n;
    *dropped = (sum > UINT16_MAX) ? UINT16_MAX : (uint16_t)sum;
}

int cli_buf_putv(char *buf, int cap, int len, uint16_t *dropped, const char *fmt, va_list ap) {
    int room = cap - len;
    int want;

    if (buf == NULL || len < 0 || len > cap)
        return len;

    /* Nothing fits, so vsnprintf is asked only how much was wanted. */
    if (room <= 1) {
        want = vsnprintf(NULL, 0, fmt, ap);
        bump(dropped, want);
        return len;
    }

    want = vsnprintf(buf + len, (size_t)room, fmt, ap);
    if (want < 0)
        return len;
    /* vsnprintf reports what it wanted to write, not what it wrote. */
    if (want >= room) {
        bump(dropped, want - (room - 1));
        return cap - 1;
    }
    return len + want;
}

int cli_buf_put(char *buf, int cap, int len, uint16_t *dropped, const char *fmt, ...) {
    va_list ap;
    int out;

    va_start(ap, fmt);
    out = cli_buf_putv(buf, cap, len, dropped, fmt, ap);
    va_end(ap);
    return out;
}

int cli_buf_note(char *buf, int cap, int len, uint16_t dropped) {
    char note[CLI_BUF_NOTE_MAX];
    int n;

    if (buf == NULL || dropped == 0u || len < 0 || len > cap)
        return len;

    n = snprintf(note, sizeof(note), "\r\n-- truncated, %u%s byte(s) lost --\r\n",
                 (unsigned)dropped, (dropped == UINT16_MAX) ? "+" : "");
    if (n < 0 || n >= cap)
        return len;
    if (n > (int)sizeof(note) - 1)
        n = (int)sizeof(note) - 1;

    if (len + n > cap)
        len = cap - n;
    memcpy(buf + len, note, (size_t)n);
    return len + n;
}
