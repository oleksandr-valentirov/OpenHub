#include "cmsis_os.h"
#include <string.h>
#include <stdio.h>
#include "cli.h"
#include "stm32h7xx_ll_usart.h"


/*
 * @retval 0 - send the rx buf to the source
 * @retval 1 - ignore
 */
uint8_t CLI_ProcessCmd(cli_data_t *cli, char c) {
    uint8_t res = 0;

    cli->response_len = 0;
    switch (c) {
        case '\n':
        case '\r':
            if (cli->cmd_len) {
                /* try to execute command */
                if (strncmp(cli->cmd_buffer, "status", 6) == 0) {
                    cli->response_buffer[0] = '\r'; cli->response_buffer[1] = '\n';
                    vTaskList(cli->response_buffer + 2);
                    cli->response_len = strlen(cli->response_buffer) + 2;
                    if (cli->response_len < 0)
                        res = 1;
                } else if (strncmp(cli->cmd_buffer, "?", 1) == 0) {
                    /* print to response buffer all available commands */
                    cli->response_len = sprintf(cli->response_buffer, 
                        "\r\nstatus - print system status\r\n"
                        "ip     - ip subcommands\r\n"
                        "?      - print available commands\r\n"
                    );
                } else if (strncmp(cli->cmd_buffer, "ip", 2) == 0) {
                    cli->response_len = sprintf(cli->response_buffer, "\r\nnot implemented\r\n");
                } else {
                    cli->response_len = sprintf(cli->response_buffer, "\r\nunknown command - %s\r\n", cli->cmd_buffer);
                }
                if (cli->response_len < 0)
                    res = 1;
                memset(cli->cmd_buffer, 0, CLI_CMD_BUF_LEN);
                cli->cmd_len = 0;
            }
            if (cli->response_len) {
                memcpy(cli->response_buffer + cli->response_len, ">>> ", 4);
                cli->response_len += 4;
            } else {
                memcpy(cli->response_buffer + cli->response_len, "\r\n>>> ", 6);
                cli->response_len += 6;
            }
            break;
        case '\t':
            /* try to print available commands based on the current input */
            break;
        default:
            if (c >= 32 && c <= 126) {
                cli->cmd_buffer[cli->cmd_len++] = c;
                cli->cmd_buffer[cli->cmd_len] = '\0';
                cli->cmd_len &= (CLI_CMD_BUF_LEN - 1);

                cli->response_buffer[0] = c;
                cli->response_len = 1;
            } else if (c == 127) {
                /* backspace */
                cli->cmd_buffer[--cli->cmd_len] = '\0';
                cli->cmd_len &= (CLI_CMD_BUF_LEN - 1);
                cli->response_buffer[0] = c;
                cli->response_len = 1;
            } else
                res = 1;  /* ignore everything that is neither a character not a supported special symbnol */
            break;
    }

    return res;
}
