#pragma once

#include <stdarg.h>
#include <stdint.h>

/**
 * @file cli_buf.h
 * @brief The console's response buffer and its bound, separated from the HAL so it can be tested.
 *
 * `cli.c` reaches the HAL and CMSIS and cannot be compiled on a PC, which is
 * why this is its own translation unit - the same reason `hubipc_str.c` is.
 *
 * radio_devices_docs/open_hub/cli.md
 */

/** Enough for the longest note, including its two line breaks and the NUL. */
#define CLI_BUF_NOTE_MAX  48

/**
 * @brief Appends one formatted line, counting the bytes that did not fit.
 * @param buf      the response buffer
 * @param cap      bytes usable for the response, the prompt's reserve excluded
 * @param len      what the buffer already holds
 * @param dropped  running total of bytes lost, saturating at 65535
 * @param fmt      printf format
 * @return         the new length
 */
int cli_buf_put(char *buf, int cap, int len, uint16_t *dropped, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/** @brief The same, for a caller that already has the argument list. */
int cli_buf_putv(char *buf, int cap, int len, uint16_t *dropped, const char *fmt, va_list ap);

/**
 * @brief Replaces the truncated tail with a line naming what was lost.
 * @param buf      the response buffer
 * @param cap      bytes usable for the response, the prompt's reserve excluded
 * @param len      what the buffer already holds
 * @param dropped  the running total; zero writes nothing
 * @return         the new length
 *
 * It overwrites rather than reserves, because the bytes it lands on were cut
 * mid-word and carry nothing, and reserving would shorten every command.
 */
int cli_buf_note(char *buf, int cap, int len, uint16_t dropped);
