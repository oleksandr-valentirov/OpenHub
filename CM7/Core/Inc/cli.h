#pragma once

#include "main.h"

#define CLI_CMD_BUF_LEN     32
#define CLI_RX_BUF_LEN      1024

typedef struct cli_data {
    int16_t response_len;
    uint8_t cmd_len;
    uint8_t new_data_flag;
    char cmd_buffer[CLI_CMD_BUF_LEN];
    char response_buffer[CLI_RX_BUF_LEN];
} cli_data_t;

uint8_t CLI_ProcessCmd(cli_data_t *cli, char c);
