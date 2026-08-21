#pragma once

#include "main.h"

/**
 * @file cli.h
 * @brief The console task and its buffers, read as an instrument rather than a feature.
 *
 * radio_devices_docs/open_hub/cli.md
 */

#define CLI_CMD_BUF_LEN     128
/* Holds a full `status` dump, so it is sized by task count, not by line length. */
#define CLI_RX_BUF_LEN      1024
/* Kept free at the end of the response for the trailing prompt. */
#define CLI_PROMPT_RESERVE  8

typedef struct cli_data {
    int16_t response_len;
    uint8_t cmd_len;
    uint8_t new_data_flag;
    char cmd_buffer[CLI_CMD_BUF_LEN];
    char response_buffer[CLI_RX_BUF_LEN];
} cli_data_t;

/**
 * @brief Feeds one received character to the line editor and runs a complete line.
 * @param cli  the console's state
 * @param c    the character received
 * @retval 1  a command ran and the response buffer holds its output
 * @retval 0  the line is still being assembled
 */
uint8_t CLI_ProcessCmd(cli_data_t *cli, char c);

/**
 * @brief The console task, blocking on its queue rather than spinning.
 * @param argument  unused, required by the CMSIS-RTOS signature
 */
void CLI_Task(void *argument);
