/* includes */
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include "cli.h"
#include "networking.h"
#include "hsem_table.h"
#include "shared_memory.h"

/* HAL/LL */
#include "stm32h7xx_hal_rng.h"

/* LWIP */
#include "netif.h"
#include "lwip/stats.h"
#include "lwip/dhcp.h"
#include "lwip/tcpip.h"

/* defines */
#define CLI_MAX_ARGS        6
#define CLI_RX_QUEUE_LEN    64
#define CLI_MAX_TASKS       12
#define RFM_REPLY_TIMEOUT_MS 500

/* types */
typedef int (*cli_handler_t)(cli_data_t *cli, int argc, char **argv);

typedef struct cli_cmd {
    const char   *name;
    uint8_t       min_args;  /* arguments after the name */
    uint8_t       max_args;
    cli_handler_t handler;
    const char   *args;
    const char   *help;
} cli_cmd_t;

/* variables */
extern struct netif gnetif;   /* defined in lwip.c */
static QueueHandle_t cli_rx_queue;
static StaticQueue_t cli_rx_queue_cb;
static uint8_t cli_rx_queue_storage[CLI_RX_QUEUE_LEN];
static m7_to_m4_rfm_request_t *rfm_shared_buffer = (m7_to_m4_rfm_request_t *)(0x38000000);

/* static functions */
static int cmd_status(cli_data_t *cli, int argc, char **argv);
static int cmd_help(cli_data_t *cli, int argc, char **argv);
static int cmd_ip(cli_data_t *cli, int argc, char **argv);
static int cmd_ping(cli_data_t *cli, int argc, char **argv);
static int cmd_cfg(cli_data_t *cli, int argc, char **argv);
static int cmd_rfm(cli_data_t *cli, int argc, char **argv);
static int cmd_lwip(cli_data_t *cli, int argc, char **argv);
static int set_server_ip_addr(cli_data_t *cli, char *server_num, char *addr, char *name);

static const cli_cmd_t commands[] = {
    {"status",  0, 0, cmd_status,  "",                       "print system status"},
    {"ip",      0, 4, cmd_ip,      "[dhcp|static|set ...]",  "show or set network config"},
    {"ping",    1, 1, cmd_ping,    "<ip addr>",              "send ping message"},
    {"cfg",     1, 1, cmd_cfg,     "<save | load>",          "config subcommand"},
    {"rfm",     1, 2, cmd_rfm,     "<dump|add|remove> <hex>","radio subcommands"},
    {"lwip",    0, 0, cmd_lwip,    "",                       "dump LwIP stack statistics"},
    {"?",       0, 0, cmd_help,    "",                       "print available commands"},
};

#define CLI_CMD_COUNT (sizeof(commands) / sizeof(commands[0]))


/* Appends to the response buffer, always leaving room for the prompt. */
static int cli_out(cli_data_t *cli, const char *fmt, ...) {
    va_list args;
    int room = CLI_RX_BUF_LEN - CLI_PROMPT_RESERVE - cli->response_len;
    int written = 0;

    if (room <= 1)
        return 0;

    va_start(args, fmt);
    written = vsnprintf(cli->response_buffer + cli->response_len, (size_t)room, fmt, args);
    va_end(args);

    if (written < 0)
        return 0;
    /* vsnprintf reports what it wanted to write, not what it wrote */
    if (written >= room)
        written = room - 1;

    cli->response_len += (int16_t)written;
    return written;
}

/* Splits the line in place. Returns argc, capped at CLI_MAX_ARGS. */
static int cli_tokenize(char *line, char **argv) {
    int argc = 0;

    while (*line && argc < CLI_MAX_ARGS) {
        while (*line == ' ')
            *line++ = '\0';
        if (*line == '\0')
            break;
        argv[argc++] = line;
        while (*line && *line != ' ')
            line++;
    }

    return argc;
}

/* Accepts both 0x-prefixed and bare hex. Returns 0 on success. */
static int parse_hex(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return 1;

    v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 0 : 16);
    if (end == s || *end != '\0')
        return 1;

    *out = (uint32_t)v;
    return 0;
}

/* Hands a request to CM4 and waits for its answer. Returns 0 on success. */
static int rfm_request(uint8_t type, uint8_t arg, const uint8_t *payload, uint8_t len) {
    uint32_t waited = 0;

    rfm_shared_buffer->request_type = type;
    rfm_shared_buffer->arg = arg;
    rfm_shared_buffer->len = len;
    if (payload && len)
        memcpy(rfm_shared_buffer->payload, payload, len);

    HAL_HSEM_FastTake(HSEM_M7_TO_M4_RFM);
    HAL_HSEM_Release(HSEM_M7_TO_M4_RFM, 0);

    while (!__HAL_HSEM_GET_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7))) {
        if (waited >= RFM_REPLY_TIMEOUT_MS)
            return 1;
        osDelay(10);
        waited += 10;
    }
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M4_TO_M7));

    return 0;
}

static char task_state_char(eTaskState state) {
    switch (state) {
        case eRunning:   return 'R';
        case eReady:     return 'r';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

static int cmd_status(cli_data_t *cli, int argc, char **argv) {
    TaskStatus_t tasks[CLI_MAX_TASKS];
    UBaseType_t count;

    UNUSED(argc);
    UNUSED(argv);

    count = uxTaskGetSystemState(tasks, CLI_MAX_TASKS, NULL);
    cli_out(cli, "\r\n%-16s %-5s %-4s %s\r\n", "task", "state", "prio", "stack free");
    for (UBaseType_t i = 0; i < count; i++) {
        cli_out(cli, "%-16s %-5c %-4lu %lu\r\n",
                tasks[i].pcTaskName,
                task_state_char(tasks[i].eCurrentState),
                (unsigned long)tasks[i].uxCurrentPriority,
                (unsigned long)tasks[i].usStackHighWaterMark);
    }

    return 0;
}

/* stats_display() floods printf and overflows this task's stack, so print the essentials */
static int cmd_lwip(cli_data_t *cli, int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    cli_out(cli, "\r\n%-7s %6s %6s %6s %6s %6s\r\n",
            "proto", "recv", "xmit", "drop", "chkerr", "err");
    cli_out(cli, "%-7s %6u %6u %6u %6u %6u\r\n", "link",
            (unsigned)lwip_stats.link.recv, (unsigned)lwip_stats.link.xmit,
            (unsigned)lwip_stats.link.drop, (unsigned)lwip_stats.link.chkerr,
            (unsigned)lwip_stats.link.err);
    cli_out(cli, "%-7s %6u %6u %6u %6u %6u\r\n", "etharp",
            (unsigned)lwip_stats.etharp.recv, (unsigned)lwip_stats.etharp.xmit,
            (unsigned)lwip_stats.etharp.drop, (unsigned)lwip_stats.etharp.chkerr,
            (unsigned)lwip_stats.etharp.err);
    cli_out(cli, "%-7s %6u %6u %6u %6u %6u\r\n", "ip",
            (unsigned)lwip_stats.ip.recv, (unsigned)lwip_stats.ip.xmit,
            (unsigned)lwip_stats.ip.drop, (unsigned)lwip_stats.ip.chkerr,
            (unsigned)lwip_stats.ip.err);
    cli_out(cli, "%-7s %6u %6u %6u %6u %6u\r\n", "icmp",
            (unsigned)lwip_stats.icmp.recv, (unsigned)lwip_stats.icmp.xmit,
            (unsigned)lwip_stats.icmp.drop, (unsigned)lwip_stats.icmp.chkerr,
            (unsigned)lwip_stats.icmp.err);
    cli_out(cli, "ip     lenerr=%u memerr=%u proterr=%u rterr=%u opterr=%u\r\n",
            (unsigned)lwip_stats.ip.lenerr, (unsigned)lwip_stats.ip.memerr,
            (unsigned)lwip_stats.ip.proterr, (unsigned)lwip_stats.ip.rterr,
            (unsigned)lwip_stats.ip.opterr);
    cli_out(cli, "heap free %lu, min ever %lu of %u bytes\r\n",
            (unsigned long)xPortGetFreeHeapSize(),
            (unsigned long)xPortGetMinimumEverFreeHeapSize(),
            (unsigned)configTOTAL_HEAP_SIZE);
    return 0;
}

static int cmd_help(cli_data_t *cli, int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    cli_out(cli, "\r\n");
    for (size_t i = 0; i < CLI_CMD_COUNT; i++)
        cli_out(cli, "%-8s %-24s %s\r\n",
                commands[i].name, commands[i].args, commands[i].help);

    return 0;
}

static int cmd_ip(cli_data_t *cli, int argc, char **argv) {
    ip4_addr_t ip, mask, gw;

    if (argc == 1) {
        cli->response_len += (int16_t)Networking_get_network_info(cli->response_buffer +
                                                                  cli->response_len);
        return 0;
    }

    if (strcmp(argv[1], "dhcp") == 0 && argc == 2) {
        LOCK_TCPIP_CORE();
        dhcp_start(&gnetif);
        UNLOCK_TCPIP_CORE();
        cli_out(cli, "\r\nDHCP client started\r\n");
        return 0;
    }

    if (strcmp(argv[1], "static") == 0 && argc == 5) {
        if (!ip4addr_aton(argv[2], &ip) || !ip4addr_aton(argv[3], &mask) ||
            !ip4addr_aton(argv[4], &gw)) {
            cli_out(cli, "\r\nError: failed to convert IP address.\r\n");
            return 0;
        }
        /* release first, or the client keeps renewing on top of the static config */
        LOCK_TCPIP_CORE();
        dhcp_release_and_stop(&gnetif);
        netif_set_addr(&gnetif, &ip, &mask, &gw);
        UNLOCK_TCPIP_CORE();
        cli_out(cli, "\r\nstatic %s\r\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc == 5)
        return set_server_ip_addr(cli, argv[2], argv[3], argv[4]);

    cli_out(cli,
        "\r\nip                              show current config\r\n"
        "ip dhcp                         switch to DHCP (default at boot)\r\n"
        "ip static <ip> <mask> <gw>      switch to a fixed address\r\n"
        "ip set s<n> <ip> <name>         remember a remote server, n in [0 - %u]\r\n",
        USER_SERVERS_MAX_NUM - 1);
    return 0;
}

static int cmd_ping(cli_data_t *cli, int argc, char **argv) {
    UNUSED(argc);

    /* prints through stdout on its own */
    Networking_ping_command(argv[1], 4, 0, 1, NULL);
    cli_out(cli, "\r\n");
    return 0;
}

static int cmd_cfg(cli_data_t *cli, int argc, char **argv) {
    UNUSED(argc);

    if (strcmp(argv[1], "save") == 0 || strcmp(argv[1], "load") == 0)
        cli_out(cli, "\r\nError: not implemented\r\n");
    else
        cli_out(cli,
            "\r\ncfg <save | load>\r\n"
            "save - saves current config to the pre-defined memory section\r\n"
            "load - loads config from the pre-defined memory section\r\n");

    return 0;
}


static int cmd_rfm(cli_data_t *cli, int argc, char **argv) {
    /* every subcommand takes one hex argument and differs only by request type */
    static const struct {
        const char *name;
        uint8_t     type;
        uint8_t     in_payload;  /* 0 - pass in arg, 1 - pass in payload */
    } ops[] = {
        {"dump",   RFM_READ_REG,      0},
        {"add",    RFM_ADD_DEVICE,    1},
        {"remove", RFM_REMOVE_DEVICE, 1},
    };
    uint32_t value = 0;
    int failed;

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (strcmp(argv[1], ops[i].name) != 0 || argc != 3)
            continue;

        if (parse_hex(argv[2], &value)) {
            cli_out(cli, "\r\nError: %s expects a hex value\r\n", ops[i].name);
            return 0;
        }

        if (ops[i].in_payload)
            failed = rfm_request(ops[i].type, 0, (const uint8_t *)&value, sizeof(value));
        else
            failed = rfm_request(ops[i].type, (uint8_t)value, NULL, 0);

        if (failed)
            cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        else if (ops[i].type == RFM_READ_REG)
            cli_out(cli, "\r\n0x%02x 0x%02x\r\n", (unsigned)value, rfm_shared_buffer->payload[0]);
        else
            cli_out(cli, "\r\nok\r\n");

        return 0;
    }

    cli_out(cli,
        "\r\nrfm <cmd> <arg>\r\n"
        "dump <reg>  - dump RFM register\r\n"
        "add <id>    - pair a device\r\n"
        "remove <id> - drop a device\r\n");
    return 0;
}

static int set_server_ip_addr(cli_data_t *cli, char *server_num, char *addr, char *name) {
    size_t name_len = strlen(name);
    uint8_t index;

    if (strlen(server_num) != 2 || server_num[0] != 's' ||
        server_num[1] < '0' || server_num[1] >= '0' + USER_SERVERS_MAX_NUM) {
        cli_out(cli, "\r\nError: server must be s0..s%u\r\n", USER_SERVERS_MAX_NUM - 1);
        return 0;
    }
    index = (uint8_t)(server_num[1] - '0');

    /* the name field has to hold a terminator too */
    if (name_len >= USER_SERVER_NAME_LEN) {
        cli_out(cli, "\r\nError: server name is longer than %i chars.\r\n",
                USER_SERVER_NAME_LEN - 1);
        return 0;
    }

    if (ip4addr_aton(addr, &(servers[index].ip)) == 0) {
        cli_out(cli, "\r\nError: failed to convert IP address.\r\n");
        return 0;
    }

    memcpy(servers[index].name, name, name_len + 1);
    cli_out(cli, "\r\nok\r\n");
    return 0;
}

/* Runs one command line. Unknown names and bad arity are reported here. */
static void cli_dispatch(cli_data_t *cli) {
    char *argv[CLI_MAX_ARGS];
    int argc = cli_tokenize(cli->cmd_buffer, argv);

    if (argc == 0)
        return;

    for (size_t i = 0; i < CLI_CMD_COUNT; i++) {
        if (strcmp(argv[0], commands[i].name) != 0)
            continue;

        if (argc - 1 < commands[i].min_args || argc - 1 > commands[i].max_args) {
            cli_out(cli, "\r\nusage: %s %s\r\n", commands[i].name, commands[i].args);
            return;
        }

        commands[i].handler(cli, argc, argv);
        return;
    }

    cli_out(cli, "\r\nError: unrecognized or incomplete cmd.\r\n%s\r\n", argv[0]);
}

/* BSP_COM_Init() already configured USART3; only the RX interrupt is missing. */
static void cli_serial_start(void) {
    cli_rx_queue = xQueueCreateStatic(CLI_RX_QUEUE_LEN, 1, cli_rx_queue_storage,
                                      &cli_rx_queue_cb);
    HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    USART3->CR1 |= USART_CR1_RXNEIE;
}

void USART3_IRQHandler(void) {
    BaseType_t woken = pdFALSE;
    uint32_t isr = USART3->ISR;
    uint8_t byte;

    /* an overrun would otherwise keep the flag set and re-enter forever */
    if (isr & USART_ISR_ORE)
        USART3->ICR = USART_ICR_ORECF;

    if (isr & USART_ISR_RXNE_RXFNE) {
        byte = (uint8_t)(USART3->RDR & 0xFFU);
        xQueueSendFromISR(cli_rx_queue, &byte, &woken);
    }

    portYIELD_FROM_ISR(woken);
}

void CLI_Task(void *argument) {
    /* static: cliTask has a 2 KB stack and cli_data_t no longer fits comfortably */
    static cli_data_t cli;
    uint8_t c;

    UNUSED(argument);

    memset(&cli, 0, sizeof(cli));
    memset(rfm_shared_buffer, 0, sizeof(m7_to_m4_rfm_request_t));
    cli_serial_start();

    for (;;) {
        /* blocks until the ISR delivers a byte, so nothing is dropped or polled */
        if (xQueueReceive(cli_rx_queue, &c, portMAX_DELAY) != pdTRUE)
            continue;

        if (CLI_ProcessCmd(&cli, (char)c) || cli.response_len <= 0)
            continue;

        for (int16_t i = 0; i < cli.response_len; i++)
            putchar(cli.response_buffer[i]);
        fflush(stdout);
    }
}

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
            if (cli->cmd_len)
                cli_dispatch(cli);
            memset(cli->cmd_buffer, 0, CLI_CMD_BUF_LEN);
            cli->cmd_len = 0;

            if (cli->response_len)
                cli_out(cli, ">>> ");
            else
                cli_out(cli, "\r\n>>> ");
            break;
        case '\t':
            /* try to print available commands based on the current input */
            res = 1;
            break;
        default:
            if (c >= 32 && c <= 126) {
                if (cli->cmd_len >= CLI_CMD_BUF_LEN - 1) {
                    res = 1;
                    break;
                }
                cli->cmd_buffer[cli->cmd_len++] = c;
                cli->cmd_buffer[cli->cmd_len] = '\0';

                cli->response_buffer[0] = c;
                cli->response_len = 1;
            } else if (c == 127) {
                /* backspace */
                if (cli->cmd_len == 0) {
                    res = 1;
                    break;
                }
                cli->cmd_buffer[--cli->cmd_len] = '\0';
                cli->response_buffer[0] = c;
                cli->response_len = 1;
            } else
                res = 1;  /* ignore everything that is neither a character not a supported special symbnol */
            break;
    }

    return res;
}
