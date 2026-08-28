#pragma once

#include "main.h"

/**
 * @file cli.h
 * @brief The console task and its buffers, read as an instrument rather than a feature.
 *
 * radio_devices_docs/open_hub/cli.md
 */

#define CLI_CMD_BUF_LEN     128
/* Sized for `devices` at the bench's roster; above about eight it truncates and says so. */
#define CLI_RX_BUF_LEN      2048
/* Kept free at the end of the response for the trailing prompt. */
#define CLI_PROMPT_RESERVE  8
/* What a command may write, the prompt's reserve excluded. */
#define CLI_RESP_CAP        (CLI_RX_BUF_LEN - CLI_PROMPT_RESERVE)

typedef struct cli_data {
    int16_t response_len;
    uint16_t dropped;           /**< bytes this command's output did not fit; ROADMAP item 107 */
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
