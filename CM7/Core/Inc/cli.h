#pragma once

#include "main.h"

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

uint8_t CLI_ProcessCmd(cli_data_t *cli, char c);
void CLI_Task(void *argument);
