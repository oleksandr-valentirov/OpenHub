/* includes */
#include "cmsis_os.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "cli.h"
#include "networking.h"
#include "crypt.h"
#include "hsem_table.h"
#include "shared_memory.h"

/* HAL/LL */
#include "stm32h7xx_ll_usart.h"
#include "stm32h7xx_hal_rng.h"

/* LWIP */
#include "netif.h"

/* variables */
static const char bad_cmd_msg[] = "\r\nError: unrecognized or incomplete cmd.\r\n";
static const char not_implemented_msg[] = "\r\nError: not implemented\r\n";
static m7_to_m4_rfm_request_t *rfm_shared_buffer = (m7_to_m4_rfm_request_t *)(0x38000000);

/* functions definitions */
static int set_server_ip_addr(char *server_num, char *addr, char *name, char *resp_buffer);


void CLI_Task(void const * argument) {
    UNUSED(argument);
    cli_data_t cli;

    memset(&cli, 0, sizeof(cli_data_t));
    memset(rfm_shared_buffer, 0, sizeof(m7_to_m4_rfm_request_t));
    for (;;) {
        if (CLI_ProcessCmd(&cli, (char)getchar()) || cli.response_len <= 0) {
            /* log an error */
        } else {
            for (uint8_t i = 0; i < cli.response_len; i++) {
                putchar(cli.response_buffer[i]);
            }
            fflush(stdout);
        }
        osDelay(pdMS_TO_TICKS(25));
    }
}


/*
 * @retval 0 - send the rx buf to the source
 * @retval 1 - ignore
 */
uint8_t CLI_ProcessCmd(cli_data_t *cli, char c) {
    uint8_t res = 0;
    char *strtok_temp = NULL;
    uint32_t device_addr = 0;
    uint8_t reg = 0;

    cli->response_len = 0;
    switch (c) {
        case '\n':
        case '\r':
            strtok_temp = strtok(cli->cmd_buffer, " ");
            if (cli->cmd_len && strtok_temp != NULL) {
                /* try to execute command */
                if (strncmp(strtok_temp, "status", strlen("status")) == 0 && strtok(NULL, " ") == NULL) {
                    /* status */
                    cli->response_buffer[0] = '\r'; cli->response_buffer[1] = '\n';
                    vTaskList(cli->response_buffer + 2);
                    cli->response_len = strlen(cli->response_buffer) + 2;
                    if (cli->response_len < 0)
                        res = 1;
                } else if (strncmp(strtok_temp, "?", strlen("?")) == 0 && strtok(NULL, " ") == NULL) {
                    /* print to response buffer all available commands */
                    cli->response_len = sprintf(cli->response_buffer, 
                        "\r\nstatus\t\t\t- print system status\r\n"
                        "ip [set]\t\t- ip subcommands\r\n"
                        "ping <ip addr>\t\t- send ping message\r\n"
                        "cfg <save | load>\t- cfg subcommand\r\n"
                        "encrypt <data>\t\t- encrypts and decrypts data with AES-128\r\n"
                        "?\t\t\t- print available commands\r\n"
                    );
                } else if (strncmp(strtok_temp, "ip", strlen("ip")) == 0) {
                    /* ip */
                    strtok_temp = strtok(NULL, " ");
                    if (strtok_temp == NULL)
                        cli->response_len = Networking_get_network_info(cli->response_buffer);
                    else if (strncmp(strtok_temp, "set", strlen("set")) == 0)
                        cli->response_len = set_server_ip_addr(strtok(NULL, " "), strtok(NULL, " "), strtok(NULL, " "), cli->response_buffer);
                } else if (strncmp(strtok_temp, "cfg", strlen("cfg")) == 0) {
                    /* cfg */
                    strtok_temp = strtok(NULL, " ");
                    if (strncmp(strtok_temp, "save", strlen("save")) == 0) {
                        cli->response_len = sprintf(cli->response_buffer, not_implemented_msg);
                    } else if (strncmp(strtok_temp, "load", strlen("load")) == 0) {
                        cli->response_len = sprintf(cli->response_buffer, not_implemented_msg);
                    } else {
                        cli->response_len = sprintf(cli->response_buffer,
                            "\r\ncfg <save | load>\r\n"
                            "save - saves current config to the pre-defined memory section\r\n"
                            "load - loads config from the pre-defined memory section\r\n"
                        );
                    }
                } else if (strncmp(strtok_temp, "encrypt", strlen("encrypt")) == 0) {
                    /* encryption */
                    cli->response_len = CRYPT_encrypt_data(strtok(NULL, " "), cli->response_buffer);
                } else if (strncmp(strtok_temp, "ping", strlen("ping")) == 0) {
                    /* ping */
                    strtok_temp = strtok(NULL, " ");
                    if (strtok_temp)
                        Networking_ping_command(strtok_temp, 4, 0, 1, NULL);
                    else
                        cli->response_len = sprintf(cli->response_buffer, "\r\nusage: ping <ip addr>\r\n");
                } else if (strncmp(strtok_temp, "rfm", strlen("rfm")) == 0) {
                    /* radio subcommands */
                    strtok_temp = strtok(NULL, " ");
                    if (strncmp(strtok_temp, "dump", strlen("dump")) == 0 && (strtok_temp = strtok(NULL, " "))) {
                        /* read RFM register content */
                        if (strtok_temp[0] == '0' && strtok_temp[1] == 'x')
                            reg = (uint8_t)strtol(strtok_temp, NULL, 0);
                        else
                            reg = (uint8_t)strtol(strtok_temp, NULL, 16);

                        rfm_shared_buffer->request_type = RFM_READ_REG;
                        rfm_shared_buffer->arg = reg;
                        HAL_HSEM_FastTake(HSEM_M7_TO_M4_RFM);
                        HAL_HSEM_Release(HSEM_M7_TO_M4_RFM, 0);

                        /* wait for the response*/
                        while (!(__HAL_HSEM_GET_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7))))
                            osDelay(10);
                        /* process response */
                        __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7));

                        cli->response_len = sprintf(cli->response_buffer + cli->response_len, 
                        "\r\n0x%02x 0x%02x\r\n", reg, rfm_shared_buffer->payload[0]);
                    } else if (strncmp(strtok_temp, "add", strlen("add")) == 0 && (strtok_temp = strtok(NULL, " "))) {
                        /* try to add device */
                        if (strtok_temp[0] == '0' && strtok_temp[1] == 'x')
                            device_addr = (uint32_t)strtol(strtok_temp, NULL, 0);
                        else
                            device_addr = (uint32_t)strtol(strtok_temp, NULL, 16);

                        rfm_shared_buffer->request_type = RFM_ADD_DEVICE;
                        // *((uint32_t *)(rfm_shared_buffer->payload)) = device_addr;
                        rfm_shared_buffer->payload[0] = 0xaa;
                        rfm_shared_buffer->payload[1] = 0xbb;
                        rfm_shared_buffer->payload[2] = 0xcc;
                        rfm_shared_buffer->payload[3] = 0xdd;
                        HAL_HSEM_FastTake(HSEM_M7_TO_M4_RFM);
                        HAL_HSEM_Release(HSEM_M7_TO_M4_RFM, 0);

                        /* wait for the response*/
                        while (!(__HAL_HSEM_GET_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7))))
                            osDelay(10);
                        /* process response */
                        __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7));
                        
                        cli->response_len = sprintf(cli->response_buffer, "\r\nok\r\n");
                    } else if (strncmp(strtok_temp, "remove", strlen("remove")) == 0 && (strtok_temp = strtok(NULL, " "))) {
                        /* remove device */
                        if (strtok_temp[0] == '0' && strtok_temp[1] == 'x')
                            device_addr = (uint32_t)strtol(strtok_temp, NULL, 0);
                        else
                            device_addr = (uint32_t)strtol(strtok_temp, NULL, 16);

                        rfm_shared_buffer->request_type = RFM_REMOVE_DEVICE;
                        memcpy(rfm_shared_buffer->payload, &device_addr, 4);
                        HAL_HSEM_FastTake(HSEM_M7_TO_M4_RFM);
                        HAL_HSEM_Release(HSEM_M7_TO_M4_RFM, 0);
                    } else {
                        cli->response_len = sprintf(cli->response_buffer, "\r\nusage: rfm <cmd> [arg]\r\n- dump <arg>\tdump RFM register\r\n");
                    }
                } else {
                    cli->response_len = sprintf(cli->response_buffer, bad_cmd_msg);
                    cli->response_len += sprintf(cli->response_buffer + cli->response_len, "%s\r\n", cli->cmd_buffer);
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
            res = 1;
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

static int set_server_ip_addr(char *server_num, char *addr, char *name, char *resp_buffer) {
    int resp_len = 0;
    uint8_t server_num_int = 0;

    if (server_num && strlen(server_num) <= 3 && server_num[0] == 's' && server_num[1] >= '0' && server_num[1] <= '2' && addr && name) {
        server_num_int = server_num[1] - 48;

        if (ip4addr_aton(addr, &(servers[server_num_int].ip)) == 0)
            return sprintf(resp_buffer, "\r\nError: failed to convert IP address.\r\n");

        if (strlen(name) <= USER_SERVER_NAME_LEN) {
            for (uint8_t i = 0; i < USER_SERVER_NAME_LEN; i++)
                servers[server_num_int].name[i] = name[i];
        } else
            return sprintf(resp_buffer, "\r\nError: Server name length is bigger the %i chars.\r\n", USER_SERVER_NAME_LEN);

    } else {
        resp_len = sprintf(resp_buffer,
            "\r\nip set s<server num> <ip> <name>\r\n"
            "ip set s0 192.168.88.37 my_main_server\r\n"
            "subcommand sets IP address of remote server\r\n\r\n"
            "server num - number of a server, range is [0 - 5]\r\n"
            "ip         - server IP address\r\n"
            "name       - server name WITHOUT spaces\r\n"
        );
    }

    return resp_len;
}
