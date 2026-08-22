/**
 * @file cli.c
 * @brief The console, which is how almost every measurement in these sources was taken.
 *
 * radio_devices_docs/open_hub/cli.md
 */
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
#include "ipc.h"
#include "keystore.h"
#include "crypto.h"
#include "hubipc.h"
#include "pairing.h"
#include "hop_v1.h"
#include "wire_v3.h"
/* For the fingerprint device list computes from the stored key. */
#include "mbedtls/sha256.h"
#include "pair_v2.h"
#include "pair_v3.h"
#include "pair_prov.h"
#include "radio_slots.h"
#include "radio_protocol.h"
#include "rng.h"
#include "crypto.h"

/* HAL/LL */
#include "telemetry.h"
#include "oht_proto.h"
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
    uint8_t       min_args;  /**< arguments after the name */
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
/* Serialises CM7's requesters across the whole transaction.
 * radio_devices_docs/open_hub/arch/ipc.md */
static ipc_msg_t   rfm_reply;

/* static functions */
static int cmd_status(cli_data_t *cli, int argc, char **argv);
static int cmd_help(cli_data_t *cli, int argc, char **argv);
static int cmd_ip(cli_data_t *cli, int argc, char **argv);
static int cmd_ping(cli_data_t *cli, int argc, char **argv);
static int cmd_cfg(cli_data_t *cli, int argc, char **argv);
static int cmd_device(cli_data_t *cli, int argc, char **argv);
static int cmd_lwip(cli_data_t *cli, int argc, char **argv);
static int cmd_rng(cli_data_t *cli, int argc, char **argv);
static int cmd_ipc(cli_data_t *cli, int argc, char **argv);
static int cmd_crypto(cli_data_t *cli, int argc, char **argv);
static int cmd_timing(cli_data_t *cli, int argc, char **argv);
static int cmd_hopprf(cli_data_t *cli, int argc, char **argv);
static int cmd_devices(cli_data_t *cli, int argc, char **argv);
static int cmd_vectors(cli_data_t *cli, int argc, char **argv);
static int cmd_telemetry(cli_data_t *cli, int argc, char **argv);
static int set_server_ip_addr(cli_data_t *cli, char *server_num, char *addr, char *name);

static const cli_cmd_t commands[] = {
    {"status",  0, 0, cmd_status,  "",                       "print system status"},
    {"ip",      0, 4, cmd_ip,      "[dhcp|static|set ...]",  "show or set network config"},
    {"ping",    1, 1, cmd_ping,    "<ip addr>",              "send ping message"},
    {"cfg",     1, 1, cmd_cfg,     "<save | load>",          "config subcommand"},
    {"device",  1, 3, cmd_device,  "<add|window|remove|list|pair|...>","devices and the radio"},
    {"devices", 0, 4, cmd_devices, "[rate <n> | cmd <dev_id> ...]",  "paired devices and their link"},
    {"vectors", 0, 0, cmd_vectors, "",                       "vector sets each core was built against"},
    {"lwip",    0, 0, cmd_lwip,    "",                       "dump LwIP stack statistics"},
    {"rng",     0, 1, cmd_rng,     "[count]",                "draw random words, report RNG health"},
    {"ipc",     0, 0, cmd_ipc,     "",                       "cross-core mailbox state"},
    {"crypto",  0, 0, cmd_crypto,  "",                       "run the crypto self-tests"},
    {"timing",  0, 0, cmd_timing,  "",                       "superframe grid and beacon jitter"},
    {"hopprf",  1, 1, cmd_hopprf,  "<32 hex chars>",         "run the hop PRF on CM4"},
    {"telem",   0, 4, cmd_telemetry, "[server <ip> <port> [token] | on | off | now]",
                                                            "the northbound telemetry link"},
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

/* Fixed-length hex into bytes, rejecting anything but exactly 2*len digits. */
static int parse_hex_bytes(const char *s, uint8_t *out, size_t len) {
    size_t i;

    if (s == NULL || strlen(s) != len * 2u)
        return 1;
    for (i = 0; i < len * 2u; i++) {
        char c = s[i];
        uint8_t nib;

        if (c >= '0' && c <= '9')      nib = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nib = (uint8_t)(c - 'A' + 10);
        else return 1;

        if ((i & 1u) == 0u) out[i / 2u]  = (uint8_t)(nib << 4);
        else                out[i / 2u] |= nib;
    }
    return 0;
}

/* The console's half of the mailbox; rfm_reply belongs to this task.
 * radio_devices_docs/open_hub/arch/ipc.md */
static int rfm_request(uint8_t type, uint8_t arg, const uint8_t *payload, uint8_t len) {
    return hub_ipc_call(type, arg, payload, len, &rfm_reply);
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

/* Draws from the guarded RNG, so a latched seed error is visible here. */
static int cmd_rng(cli_data_t *cli, int argc, char **argv) {
    static const char *const names[] = {"ok", "bad argument", "seed error",
                                        "clock error", "timeout"};
    long count = (argc > 1) ? strtol(argv[1], NULL, 10) : 4;
    rng_status_t st = RNG_OK;

    if (count < 1 || count > 32)
        count = 4;

    cli_out(cli, "\r\nRNG_SR on entry 0x%08lX%s\r\n",
            (unsigned long)RNG->SR,
            ((RNG->SR & RNG_SR_SEIS) != 0u) ? " (SEIS latched while idle)" : "");
    for (long i = 0; i < count; i++) {
        uint32_t w = 0;

        st = rng_word(&w);
        if (st != RNG_OK) {
            cli_out(cli, "draw %ld: %s (SR=0x%08lX)\r\n",
                    i, names[st], (unsigned long)rng_last_status_reg());
            break;
        }
        cli_out(cli, "draw %ld: 0x%08lX\r\n", i, (unsigned long)w);
    }

    cli_out(cli, "RNG_SR now 0x%08lX, CR 0x%08lX -- %s\r\n",
            (unsigned long)RNG->SR, (unsigned long)RNG->CR,
            (st == RNG_OK) ? "healthy" : "FAULT");
    return 0;
}


static int cmd_ipc(cli_data_t *cli, int argc, char **argv) {
    uint32_t rh, rt, sh, st;

    UNUSED(argc);
    UNUSED(argv);

    ipc_ring_state(&shared_ipc.req, &rh, &rt);
    ipc_ring_state(&shared_ipc.rsp, &sh, &st);

    cli_out(cli, "\r\nmagic 0x%08lX ver %lu -- %s\r\n",
            (unsigned long)shared_ipc.magic, (unsigned long)shared_ipc.version,
            ipc_ready() ? "ready" : "NOT READY");
    cli_out(cli, "req m7->m4  head %lu tail %lu (%lu queued)\r\n",
            (unsigned long)rh, (unsigned long)rt,
            (unsigned long)((rh - rt) & (IPC_RING_SLOTS - 1u)));
    cli_out(cli, "rsp m4->m7  head %lu tail %lu (%lu queued)\r\n",
            (unsigned long)sh, (unsigned long)st,
            (unsigned long)((sh - st) & (IPC_RING_SLOTS - 1u)));
    /* Non-zero means CM4 is answering after the caller gave up. */
    cli_out(cli, "stale replies dropped: %lu\r\n",
            (unsigned long)ipc_stale_replies());
    return 0;
}


/* The only on-target check available while there is no device to talk to. */
static int cmd_crypto(cli_data_t *cli, int argc, char **argv) {
    int failures = 0;

    UNUSED(argc);
    UNUSED(argv);

    cli_out(cli, "\r\n");
    for (int t = 0; t < CRYPTO_TEST_COUNT; t++) {
        uint32_t t0 = osKernelGetTickCount();
        int rc = crypto_run_test((crypto_test_t)t);
        uint32_t ms = osKernelGetTickCount() - t0;

        if (rc != 0)
            failures++;
        cli_out(cli, "%-22s %-4s %4lu ms", crypto_test_name((crypto_test_t)t),
                (rc == 0) ? "ok" : "FAIL", (unsigned long)ms);
        if (rc != 0)
            cli_out(cli, "  rc=%d", rc);
        cli_out(cli, "\r\n");
    }
    cli_out(cli, "%d/%d passed\r\n", CRYPTO_TEST_COUNT - failures, CRYPTO_TEST_COUNT);
    return 0;
}


/* Beacon jitter lands in every device's drift.
 * radio_devices_docs/open_hub/cli.md */
static int cmd_timing(cli_data_t *cli, int argc, char **argv) {
    ipc_timing_t t;
    int rc;

    UNUSED(argc);
    UNUSED(argv);

    rc = rfm_request(IPC_REQ_GET_TIMING, 0, NULL, 0);
    if (rc < 0) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    if (rc != IPC_ST_OK || rfm_reply.len < sizeof(t)) {
        cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        return 0;
    }
    memcpy(&t, rfm_reply.payload, sizeof(t));

    cli_out(cli, "\r\nsuperframe %lu, period %lu us nominal\r\n",
            (unsigned long)t.superframe, (unsigned long)t.period_us);
    cli_out(cli, "beacon late: last %lu us, min %lu, max %lu (spread %lu)\r\n",
            (unsigned long)t.late_last_us, (unsigned long)t.late_min_us,
            (unsigned long)t.late_max_us,
            (unsigned long)(t.late_max_us - t.late_min_us));
    /* The grid steps ticks, not microseconds.
     * radio_devices_docs/open_hub/cli.md */
    cli_out(cli, "clock %+ld ppm vs nominal, grid steps %lu ticks\r\n",
            (long)t.calib_ppm, (unsigned long)t.period_tk);
    cli_out(cli, "calib: %lu windows, %lu rejected%s\r\n",
            (unsigned long)t.calib_windows, (unsigned long)t.calib_rejects,
            t.calib_windows ? "" : "  <- LSE never measured, period is nominal");
    /* A window lands every ~8 ms; past a second the ppm above is a memory.
     * radio_devices_docs/open_hub/cli.md */
    if (t.calib_windows) {
        unsigned long age_ms = (unsigned long)(t.calib_age_tk / 1000u);

        cli_out(cli, "       last window %lu ms ago%s\r\n", age_ms,
                age_ms > 1000u ? "  <- STALE, the ppm above is not current" : "");
    }
    /* A wide spread means the reference is mismeasured, not that the clock moved.
     * radio_devices_docs/open_hub/cli.md */
    cli_out(cli, "       spread %+ld..%+ld ppm\r\n",
            (long)t.calib_ppm_min, (long)t.calib_ppm_max);
    /* Two reads far apart give the tick rate against the host clock. */
    cli_out(cli, "       last window spans %lu..%lu ticks\r\n",
            (unsigned long)t.span_lo, (unsigned long)t.span_hi);
    /* No figure quoted: CM4 counted against CM4's limit.
     * radio_devices_docs/open_hub/cli.md */
    if (t.late_over)
        cli_out(cli, "*** %lu beacon(s) left late past the cost limit - something"
                     " in the radio superloop is expensive\r\n",
                (unsigned long)t.late_over);
    cli_out(cli, "ticks %lu\r\n", (unsigned long)t.now_tk);
    return 0;
}


/* Exposes what the accelerator computes, byte for byte.
 * radio_devices_docs/radio/hopping.md */
static int cmd_hopprf(cli_data_t *cli, int argc, char **argv) {
    uint8_t in[16];
    int rc;

    UNUSED(argc);

    if (strlen(argv[1]) != 32) {
        cli_out(cli, "\r\nhopprf <32 hex chars>\r\n");
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        char b[3] = { argv[1][2 * i], argv[1][2 * i + 1], 0 };
        in[i] = (uint8_t)strtol(b, NULL, 16);
    }

    rc = rfm_request(IPC_REQ_HOP_PRF, 0, in, sizeof(in));
    if (rc < 0) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    if (rc != IPC_ST_OK || rfm_reply.len < 16) {
        cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        return 0;
    }

    cli_out(cli, "\r\n");
    for (int i = 0; i < 16; i++)
        cli_out(cli, "%02x", rfm_reply.payload[i]);
    cli_out(cli, "\r\n");
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


static const char *pair_state_name(uint8_t st) {
    switch (st) {
    case RADIO_PAIR_IDLE:    return "idle";
    case RADIO_PAIR_LISTEN:  return "listen";
    case RADIO_PAIR_QUIESCE: return "quiesce";
    default:                 return "?";
    }
}


/* The operator's view of the network, in both RSSI directions.
 * radio_devices_docs/open_hub/cli.md */
static int cmd_devices(cli_data_t *cli, int argc, char **argv) {
    ipc_exchange_state_t x;
    uint32_t now = 0;
    uint8_t total = 0;
    uint8_t i;
    int rc;

    /* Aimed at one device already out there, unlike `rate`, which grants the next. */
    if (argc >= 4 && strcmp(argv[1], "cmd") == 0) {
        ipc_device_cmd_t c;
        int n = (argc == 5) ? atoi(argv[4]) : 0;

        memset(&c, 0, sizeof(c));
        c.dev_id  = (uint32_t)strtoul(argv[2], NULL, 0);
        c.repeats = 4;
        if (strcmp(argv[3], "rejoin") == 0 && argc == 4) {
            c.cmd = RADIO_CMD_REJOIN;
        } else if (strcmp(argv[3], "rate") == 0 && argc == 5 && n >= 1 && n <= 255) {
            c.cmd          = RADIO_CMD_SET_RATE;
            c.report_every = (uint8_t)n;
        } else {
            cli_out(cli, "\r\nUsage: devices cmd <dev_id> rejoin | rate <1..255>\r\n");
            return 0;
        }
        rc = rfm_request(IPC_REQ_SET_DEVICE_PARAM, 0, (const uint8_t *)&c,
                         (uint8_t)sizeof(c));
        if (rc != IPC_ST_OK) {
            cli_out(cli, "\r\nError: no such device, status %d\r\n", rc);
            return 0;
        }
        /* Nothing on the wire acknowledges it, so this says queued, not delivered. */
        cli_out(cli, "\r\nqueued for 0x%08lX, riding the next %u downlinks\r\n",
                (unsigned long)c.dev_id, c.repeats);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "rate") == 0) {
        int n = atoi(argv[2]);

        if (n < 1 || n > 255) {
            cli_out(cli, "\r\nError: rate is 1..255 superframes\r\n");
            return 0;
        }
        rc = rfm_request(IPC_REQ_SET_REPORT_RATE, (uint8_t)n, NULL, 0);
        if (rc != IPC_ST_OK) {
            cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
            return 0;
        }
        pairing_set_report_every((uint8_t)n);
        /* Granted at the next pairing, not to devices already out there.
         * radio_devices_docs/open_hub/cli.md */
        cli_out(cli, "\r\nreport rate %d superframes, granted at the next pairing\r\n", n);
        return 0;
    }
    if (argc != 1) {
        cli_out(cli, "\r\nUsage: devices [rate <n>] | cmd <dev_id> rejoin | cmd <dev_id> rate <n>\r\n");
        return 0;
    }

    rc = rfm_request(IPC_REQ_GET_EXCHANGE, 0, NULL, 0);
    if (rc < 0) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    if (rc != IPC_ST_OK || rfm_reply.len < sizeof(x)) {
        cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        return 0;
    }
    memcpy(&x, rfm_reply.payload, sizeof(x));

    {
        ipc_timing_t t;

        if (rfm_request(IPC_REQ_GET_TIMING, 0, NULL, 0) == IPC_ST_OK &&
            rfm_reply.len >= sizeof(t)) {
            memcpy(&t, rfm_reply.payload, sizeof(t));
            now = t.superframe;
        }
    }

    cli_out(cli, "\r\n");
    if (x.devices == 0u) {
        cli_out(cli, "no devices paired\r\n");
    } else {
        cli_out(cli, "slot  dev_id      rssi up/down  supply  uptime   ok/bad     last seen\r\n");
    }

    for (i = 0; i < x.devices; i++) {
        ipc_device_report_t d;
        char age[16];
        char supply[12];

        rc = rfm_request(IPC_REQ_GET_DEVICE_INFO, i, NULL, 0);
        if (rc != IPC_ST_OK || rfm_reply.len < sizeof(d))
            break;
        memcpy(&d, rfm_reply.payload, sizeof(d));
        total = d.total;

        /* Never an age counted from zero, which reads as "just now" at boot.
         * radio_devices_docs/open_hub/cli.md */
        if (d.frames_ok == 0u)
            snprintf(age, sizeof(age), "never");
        else
            snprintf(age, sizeof(age), "%lus ago",
                     (unsigned long)((now - d.last_superframe) *
                                     (SUPERFRAME_US / 1000000u)));

        /* A rail never measured and a rail reading 0 mV print alike otherwise.
         * radio_devices_docs/open_hub/cli.md */
        if (d.flags & RADIO_REPORT_FLAG_SUPPLY_STALE)
            snprintf(supply, sizeof(supply), "%6s", "--");
        else
            snprintf(supply, sizeof(supply), "%4umV", d.supply_mv);

        cli_out(cli, "%4u  0x%08lX  %4d/%-5d %s  %s  %6lus  %lu/%lu  %s\r\n",
                d.slot, (unsigned long)d.dev_id, d.rssi_up, d.rssi_down,
                (d.flags & RADIO_REPORT_FLAG_RSSI_STALE) ? "stale" : "     ",
                supply, (unsigned long)d.uptime_s,
                (unsigned long)d.frames_ok, (unsigned long)d.frames_bad, age);

        /* Granted against observed, never a grant restated as a measurement.
         * radio_devices_docs/open_hub/cli.md */
        {
            char asked[56];
            char applied[12];
            unsigned want = d.report_every;

            /* An acked command is what the hub believes; a riding one is not. */
            if (d.cmd_state == 2u) {
                /* 0 is not a rate a SET_RATE can apply, so it cannot mean one. */
                if (d.ack_arg == 0u)
                    snprintf(applied, sizeof(applied), "unstated");
                else
                    snprintf(applied, sizeof(applied), "%u", d.ack_arg);
                snprintf(asked, sizeof(asked),
                         "grant %u, commanded %u acked, applied %s",
                         d.report_every, d.cmd_every, applied);
                want = d.cmd_every;
            } else if (d.cmd_state == 1u) {
                snprintf(asked, sizeof(asked), "grant %u, commanded %u riding",
                         d.report_every, d.cmd_every);
            } else {
                snprintf(asked, sizeof(asked), "grant %u", d.report_every);
            }
            if (d.cyc_n == 0u)
                cli_out(cli, "      cadence: %s, not yet observed\r\n", asked);
            else
                cli_out(cli, "      cadence: %s, seen every %u"
                             " (mean %lu.%lu over %u gaps)%s\r\n",
                        asked, d.cyc_min,
                        (unsigned long)(d.cyc_sum / d.cyc_n),
                        (unsigned long)((d.cyc_sum * 10u / d.cyc_n) % 10u),
                        d.cyc_n,
                        (want != 0u && d.cyc_min != want)
                            ? "  <- not what the device does" : "");
        }
    }

    /* The count CM4 reports and the count actually printed.
     * radio_devices_docs/open_hub/cli.md */
    if (x.devices != 0u && total != x.devices)
        cli_out(cli, "listing incomplete: %u of %u\r\n", total, x.devices);

    cli_out(cli, "\r\nexchange %s, granting a report every %u superframes\r\n",
            (x.state == RADIO_EX_IDLE)      ? "idle" :
            (x.state == RADIO_EX_WAIT_RSP)  ? "waiting on CM7 (derive)" :
            (x.state == RADIO_EX_SENT_RSP)  ? "sent PAIR_RSP" :
            (x.state == RADIO_EX_WAIT_KEYS) ? "waiting on CM7 (confirm)" :
                                              "accepted",
            x.report_every);
    cli_out(cli, "req %lu -> rsp %lu -> conf %lu -> accept %lu, %lu paired\r\n",
            (unsigned long)x.reqs_forwarded, (unsigned long)x.rsp_sent,
            (unsigned long)x.confs_forwarded, (unsigned long)x.accepts_sent,
            (unsigned long)x.paired);
    if (x.cm7_refused || x.timeouts || x.tx_err || x.seal_err)
        cli_out(cli, "refused %lu, timed out %lu, tx err %lu, seal err %lu\r\n",
                (unsigned long)x.cm7_refused, (unsigned long)x.timeouts,
                (unsigned long)x.tx_err, (unsigned long)x.seal_err);
    /* Malformed and bad-tag stay apart: the air wrong against the key wrong.
     * radio_devices_docs/open_hub/cli.md */
    {
        ipc_downlink_state_t dl;

        if (rfm_request(IPC_REQ_GET_DOWNLINK, 0, NULL, 0) == IPC_ST_OK &&
            rfm_reply.len >= sizeof(dl)) {
            memcpy(&dl, rfm_reply.payload, sizeof(dl));
            /* Opportunities beside sends, which "sent 0" cannot separate.
             * radio_devices_docs/open_hub/cli.md */
            cli_out(cli, "downlink %lu sent of %lu opportunities, next slot %u"
                         " (%lu seal err, %lu tx err, %lu no device)\r\n",
                    (unsigned long)dl.sent, (unsigned long)dl.opportunities,
                    dl.next_slot, (unsigned long)dl.seal_err,
                    (unsigned long)dl.tx_err, (unsigned long)dl.no_device);
            /* Only ever 0, so it says nothing yet. ROADMAP item 36 */
            if (dl.nonce_refused != 0u)
                cli_out(cli, "downlink: %lu seal(s) refused, the nonce tuple was"
                             " not new\r\n", (unsigned long)dl.nonce_refused);
            /* Sent is not delivered: no uplink field acknowledges a command. */
            cli_out(cli, "  cmds %lu sent, %lu acked, %lu lost, %lu replaced\r\n",
                    (unsigned long)dl.cmd_sent, (unsigned long)dl.cmd_acked,
                    (unsigned long)dl.cmd_lost, (unsigned long)dl.cmd_replaced);
            /* The channel: every counter above reads success on a wrong one. */
            if (dl.sent)
                cli_out(cli, "last downlink sf %lu on %lu Hz (%lu prf err)\r\n",
                        (unsigned long)dl.last_superframe,
                        (unsigned long)dl.last_hz, (unsigned long)dl.prf_err);
        }
    }
    {
        const pairing_init_stats_t *pi = pairing_init_get_stats();

        /* z1_derivations beside the frame counts, so the claim is measured.
         * radio_devices_docs/open_hub/cli.md */
        if (pi->armed || pi->built || pi->derive_failed)
            cli_out(cli, "pair_init %s: %lu built, %lu pushed, %lu push err,"
                         " %lu derive err, z1 x%lu, last sf %lu\r\n",
                    pi->armed ? "armed" : "idle",
                    (unsigned long)pi->built, (unsigned long)pi->pushed,
                    (unsigned long)pi->push_failed,
                    (unsigned long)pi->derive_failed,
                    (unsigned long)pi->z1_derivations,
                    (unsigned long)pi->last_superframe);
        {
            /* CM4's half: pushed only proves CM7 handed the frame over.
             * radio_devices_docs/open_hub/cli.md */
            ipc_pair_init_state_t ps;

            if (rfm_request(IPC_REQ_GET_PAIR_INIT, 0, NULL, 0) == IPC_ST_OK &&
                rfm_reply.len >= sizeof(ps)) {
                memcpy(&ps, rfm_reply.payload, sizeof(ps));
                cli_out(cli, "  cm4: %lu given, %lu sent, %lu missed, %lu tx err,"
                             " %lu replaced, last sf %lu, pending %lu\r\n",
                        (unsigned long)ps.given, (unsigned long)ps.sent,
                        (unsigned long)ps.missed, (unsigned long)ps.tx_err,
                        (unsigned long)ps.replaced,
                        (unsigned long)ps.last_sent_sf,
                        (unsigned long)ps.pending_sf);
                /* Frf steps of 61.03515625 Hz; 866.5 MHz is 0xD8A000. */
                cli_out(cli, "  cm4 readback: RegFrf %06lX (%lu Hz), "
                             "RegPayloadLength %u\r\n",
                        (unsigned long)ps.frf,
                        (unsigned long)((uint64_t)ps.frf * 32000000ull / 524288ull),
                        ps.payload_len);
            }
        }
        if (pi->last_len) {
            cli_out(cli, "  last frame: ");
            for (uint8_t b = 0; b < pi->last_len; b++)
                cli_out(cli, "%02X", pi->last_frame[b]);
            cli_out(cli, "\r\n");
        }
    }
    cli_out(cli, "uplink windows %lu, sync %lu\r\n",
            (unsigned long)x.uplink_windows, (unsigned long)x.uplink_sync);
    {
        ipc_rx_diag_t ud;

        if (rfm_request(IPC_REQ_GET_RXDIAG, 0, NULL, 0) == IPC_ST_OK &&
            rfm_reply.len >= sizeof(ud)) {
            memcpy(&ud, rfm_reply.payload, sizeof(ud));
            cli_out(cli, "uplink level: peak %d dBm, floor %d dBm\r\n",
                    ud.up_rssi_peak, ud.up_rssi_floor);
            /* No uplink bucket can hold it, and its denominator differs.
             * radio_devices_docs/open_hub/cli.md */
            cli_out(cli, "crc err %lu, every channel, since the last pairing"
                         " window opened\r\n", (unsigned long)ud.crc_err);
        }
    }
    {
        ipc_device_report_t last;
        uint32_t shrt = 0, tick = 0;
        uint32_t seen = pairing_uplink_events(&shrt, &tick, &last);

        /* Both sides of one number: CM4 sent ok minus drops, CM7 handled seen. */
        cli_out(cli, "uplink events: cm4 sent %lu (%lu dropped), cm7 handled %lu"
                     " (%lu short)\r\n",
                (unsigned long)(x.uplink_ok - x.uplink_evt_drop),
                (unsigned long)x.uplink_evt_drop, (unsigned long)seen,
                (unsigned long)shrt);
        {
            uint32_t tmo = 0;
            uint32_t rings = pairing_doorbells(&tmo);

            /* Zero rings with events handled means the poll did all the work. */
            cli_out(cli, "  wakes: %lu doorbell, %lu timeout\r\n",
                    (unsigned long)rings, (unsigned long)tmo);
        }
        if (seen != 0u)
            cli_out(cli, "  last: dev 0x%08lX slot %u, sf %lu at +%lu us,"
                         " handled %lu ms ago\r\n",
                    (unsigned long)last.dev_id, last.slot,
                    (unsigned long)last.last_superframe,
                    (unsigned long)last.arrival_us,
                    (unsigned long)(osKernelGetTickCount() - tick));
    }
    cli_out(cli, "uplink %lu seen, %lu ok, %lu unknown slot, %lu malformed,"
                 " %lu bad tag, %lu replay\r\n",
            (unsigned long)x.uplink_frames, (unsigned long)x.uplink_ok,
            (unsigned long)x.uplink_bad_slot, (unsigned long)x.uplink_bad_frame,
            (unsigned long)x.uplink_bad_tag, (unsigned long)x.uplink_replay);
    if (x.aead_selftest != 0u)
        cli_out(cli, "WARNING: frame cipher self-test failed (check %u)\r\n",
                x.aead_selftest);

    {
        const pairing_stats_t *ps = pairing_get_stats();

        cli_out(cli, "cm7: %lu req, %lu derived, %lu paired, %lu installed\r\n",
                (unsigned long)ps->reqs, (unsigned long)ps->derived,
                (unsigned long)ps->paired, (unsigned long)ps->installed);
        if (ps->not_enrolled || ps->bad_fingerprint || ps->zero_nonce ||
            ps->repeat_nonce || ps->bad_confirm || ps->no_hub_key ||
            ps->derive_failed || ps->store_failed || ps->timed_out)
            cli_out(cli, "cm7 refusals: enrol %lu, fingerprint %lu, nonce %lu/%lu, "
                    "confirm %lu, hubkey %lu, derive %lu, store %lu, timeout %lu\r\n",
                    (unsigned long)ps->not_enrolled, (unsigned long)ps->bad_fingerprint,
                    (unsigned long)ps->zero_nonce, (unsigned long)ps->repeat_nonce,
                    (unsigned long)ps->bad_confirm, (unsigned long)ps->no_hub_key,
                    (unsigned long)ps->derive_failed, (unsigned long)ps->store_failed,
                    (unsigned long)ps->timed_out);
        /* Both halves: a wrong key and a wrong domain refuse identically.
         * radio_devices_docs/open_hub/cli.md */
        if (ps->bad_fingerprint) {
            cli_out(cli, "computed fp: ");
            for (int b = 0; b < 32; b++)
                cli_out(cli, "%02x", ps->last_fp[b]);
            cli_out(cli, "\r\nof pubkey: ");
            for (int b = 0; b < 8; b++)
                cli_out(cli, "%02x", ps->last_pubkey[b]);
            cli_out(cli, "...\r\n");
        }
    }
    return 0;
}


/* Which vector sets each core was built against; never tampering.
 * radio_devices_docs/open_hub/arch/build-and-generation.md */
static int cmd_vectors(cli_data_t *cli, int argc, char **argv) {
    ipc_vectors_t v;
    int rc;

    (void)argc; (void)argv;

    /* Named on every run, so "ok" is not carried away as "validated".
     * radio_devices_docs/open_hub/arch/build-and-generation.md */
    cli_out(cli, "\r\ncompares what each core was BUILT with; does not validate the values\r\n");
    cli_out(cli, "           CM7                CM4\r\n");

    rc = rfm_request(IPC_REQ_GET_VECTORS, 0, NULL, 0);
    if (rc < 0 || rfm_reply.len < sizeof(v)) {
        cli_out(cli, "pair_v%-3d  %-18s (CM4 did not answer)\r\n",
                PAIR_VECTORS_VERSION, PAIR_VECTORS_DIGEST);
        cli_out(cli, "wire       %s\r\n", WIRE_VECTORS_DIGEST);
        return 0;
    }
    memcpy(&v, rfm_reply.payload, sizeof(v));
    /* CM4 does not compile the wire set: a blank column, never CM7's twice. */
    v.pair[sizeof(v.pair) - 1] = '\0';
    v.hop[sizeof(v.hop) - 1] = '\0';

    cli_out(cli, "pair_v%-3d  %-18s %-18s %s\r\n", PAIR_VECTORS_VERSION,
            PAIR_VECTORS_DIGEST, v.pair,
            strcmp(PAIR_VECTORS_DIGEST, v.pair) == 0 ? "ok" : "MISMATCH");
    cli_out(cli, "hop_v1     %-18s %-18s %s\r\n", HOP_VECTORS_DIGEST, v.hop,
            strcmp(HOP_VECTORS_DIGEST, v.hop) == 0 ? "ok" : "MISMATCH");
    cli_out(cli, "wire       %-18s %-18s\r\n", WIRE_VECTORS_DIGEST, "-");
    /* CM7-only sets, listed anyway: an omission reads as a set that does not exist.
     * radio_devices_docs/open_hub/arch/build-and-generation.md */
    cli_out(cli, "pair_v%-3d  %-18s %-18s\r\n", PAIR_V3_VECTORS_VERSION,
            PAIR_V3_VECTORS_DIGEST, "-");
    cli_out(cli, "pair_prov  %-18s %-18s\r\n", PAIR_PROV_VECTORS_DIGEST, "-");

    if (strcmp(PAIR_VECTORS_DIGEST, v.pair) != 0 ||
        strcmp(HOP_VECTORS_DIGEST, v.hop) != 0)
        cli_out(cli, "\r\nOne core is stale. Flash both, then reset.\r\n");
    return 0;
}

static int cmd_device_pair(cli_data_t *cli) {
    ipc_pair_state_t p;
    int rc = rfm_request(IPC_REQ_GET_PAIR_STATE, 0, NULL, 0);

    if (rc < 0) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    if (rc != IPC_ST_OK || rfm_reply.len < sizeof(p)) {
        cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        return 0;
    }
    memcpy(&p, rfm_reply.payload, sizeof(p));

    cli_out(cli, "\r\npairing %s, device 0x%08lx\r\n",
            pair_state_name(p.state), (unsigned long)p.dev_id);
    cli_out(cli, "window %lu ms left\r\n", (unsigned long)p.window_left_ms);
    cli_out(cli, "quiesce %u superframes left, resume at %lu\r\n",
            (unsigned)p.quiesce_left, (unsigned long)p.resume_at);
    {
        static const char *why[] = {"-", "version", "type", "length", "ids",
                                    "no window", "busy",
                                    "net id", "hub id", "dev id"};
        cli_out(cli, "join reqs %lu seen, %lu dropped (last: %s)\r\n",
                (unsigned long)p.reqs_seen, (unsigned long)p.reqs_dropped,
                p.reqs_drop_last < RADIO_DROP_COUNT ? why[p.reqs_drop_last] : "?");
    }
    /* Below the frame layer; the three counters together are the diagnosis.
     * radio_devices_docs/open_hub/radio/configuration.md */
    {
        ipc_rx_diag_t d;

        if (rfm_request(IPC_REQ_GET_RXDIAG, 0, NULL, 0) == IPC_ST_OK &&
            rfm_reply.len >= sizeof(d)) {
            memcpy(&d, rfm_reply.payload, sizeof(d));
            /* Since this window opened, and every channel. ROADMAP item 13 */
            cli_out(cli, "rx since this window opened, every channel:"
                         " sync %lu, crc err %lu, frames %lu\r\n",
                    (unsigned long)d.sync_match, (unsigned long)d.crc_err,
                    (unsigned long)d.frames);
            /* Each one is a window that would otherwise have ended there. */
            if (d.flushes != 0u)
                cli_out(cli, "rx: %lu receiver restart(s) on an undrainable FIFO\r\n",
                        (unsigned long)d.flushes);
            /* Below the sync word, which every counter above needs first.
             * radio_devices_docs/open_hub/radio/configuration.md */
            cli_out(cli, "rx level: peak %d dBm, floor %d dBm, %lu samples\r\n",
                    d.rssi_peak, d.rssi_floor, (unsigned long)d.rssi_samples);
            /* Printed on any identity refusal: the field name settles nothing. */
            if (d.drop_head[0]) {
                cli_out(cli, "cm4 saw head:");
                for (int b = 0; b < 16; b++)
                    cli_out(cli, " %02x", d.drop_head[b]);
                cli_out(cli, "\r\ncm4 saw key: ");
                for (int b = 0; b < 8; b++)
                    cli_out(cli, "%02x", d.drop_key[b]);
                cli_out(cli, "...\r\n");
            }
            if (d.drop_net_id || d.drop_hub_id || d.drop_dev_id) {
                cli_out(cli, "last refused ids: net %04x, hub %08lx, dev %08lx\r\n",
                        d.drop_net_id, (unsigned long)d.drop_hub_id,
                        (unsigned long)d.drop_dev_id);
            }
            if (d.last_len)
                cli_out(cli, "last frame: %u bytes, type %02x, %d dBm, superframe %lu\r\n",
                        d.last_len, d.last_type, d.last_rssi,
                        (unsigned long)d.last_superframe);
        }
    }
    cli_out(cli, "join regions %lu, beacons %lu, tx err %lu\r\n",
            (unsigned long)p.join_regions, (unsigned long)p.join_beacons,
            (unsigned long)p.join_tx_err);
    if (p.beacon_err) {
        static const char *bwhy[] = {"-", "build", "hop prf", "retune", "tx"};
        cli_out(cli, "beacon errors %lu (last: %s)\r\n", (unsigned long)p.beacon_err,
                p.beacon_err_last < 5u ? bwhy[p.beacon_err_last] : "?");
    }
    cli_out(cli, "beacons %lu (%lu announce), %lu silent, %lu refused\r\n",
            (unsigned long)p.data_beacons, (unsigned long)p.announce_beacons,
            (unsigned long)p.silent_frames, (unsigned long)p.quiesce_refused);
    if (p.beacon_err || p.quiesce_lost)
        cli_out(cli, "beacon errors %lu, pairings that got no clear air %lu\r\n",
                (unsigned long)p.beacon_err, (unsigned long)p.quiesce_lost);
    return 0;
}

static int cmd_device_store(cli_data_t *cli) {
    ipc_store_state_t k;
    int rc = rfm_request(IPC_REQ_GET_STORE, 0, NULL, 0);

    if (rc < 0) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    if (rc != IPC_ST_OK || rfm_reply.len < sizeof(k)) {
        cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        return 0;
    }
    memcpy(&k, rfm_reply.payload, sizeof(k));

    cli_out(cli, "\r\ncounter %lu, reserved to %lu (%ld ahead)\r\n",
            (unsigned long)k.counter, (unsigned long)k.reserved,
            (long)(int32_t)(k.reserved - k.counter));
    cli_out(cli, "flash: %lu writes, %lu errors, %lu slots left\r\n",
            (unsigned long)k.writes, (unsigned long)k.errors,
            (unsigned long)k.slots_left);
    if (k.errors || k.unreserved)
        cli_out(cli, "STOPPED: %lu superframes sent nothing to avoid nonce reuse"
                     " - reboot to compact\r\n", (unsigned long)k.unreserved);
    return 0;
}

static const char *ks_state_name(uint8_t st) {
    switch (st) {
    case KS_STATE_ENROLLED: return "enrolled";
    case KS_STATE_PAIRED:   return "paired";
    case KS_STATE_DELETED:  return "removed";
    default:                return "?";
    }
}

static int cmd_device_list(cli_data_t *cli) {
    uint32_t n = ks_count();
    uint32_t live = 0;

    cli_out(cli, "\r\nid        slot state    fingerprint\r\n");
    for (uint32_t i = 0; i < n; i++) {
        const ks_record_t *r = ks_at(i);

        if (r == NULL || r->state == KS_STATE_DELETED)
            continue;
        live++;
        cli_out(cli, "%08lx  %3u %-8s ", (unsigned long)r->dev_id,
                (unsigned)r->slot, ks_state_name(r->state));
        /* Display only, eight bytes of it; the check above compares the point.
         * radio_devices_docs/open_hub/cli.md */
        {
            uint8_t fp[KS_FINGERPRINT_BYTES];

            if (mbedtls_sha256(r->pubkey, sizeof(r->pubkey), fp, 0) != 0) {
                cli_out(cli, "<sha256 failed>\r\n");
                continue;
            }
            for (int b = 0; b < 8; b++)
                cli_out(cli, "%02x", fp[b]);
        }
        cli_out(cli, "...\r\n");
    }
    /* Counts what was printed, not what the cache holds.
     * radio_devices_docs/open_hub/cli.md */
    cli_out(cli, "%lu enrolled, flash: %lu writes, %lu errors, %lu slots left\r\n",
            (unsigned long)live, (unsigned long)ks_writes(),
            (unsigned long)ks_errors(), (unsigned long)ks_slots_left());
    if (ks_errors())
        cli_out(cli, "last flash error 0x%08lx\r\n",
                (unsigned long)ks_last_flash_error());
    if (ks_stale_format())
        cli_out(cli, "%lu slot(s) hold records of an older format and are skipped\r\n",
                (unsigned long)ks_stale_format());
    return 0;
}

/* Enrol, then open the window, in that order.
 * radio_devices_docs/open_hub/cli.md */
static int cmd_device_add(cli_data_t *cli, char **argv) {
    uint8_t pubkey[KS_PUBKEY_BYTES];
    uint32_t dev_id = 0;
    uint8_t slot = 0;
    int rc;

    if (parse_hex(argv[2], &dev_id)) {
        cli_out(cli, "\r\nError: device id must be hex\r\n");
        return 0;
    }
    if (parse_hex_bytes(argv[3], pubkey, sizeof(pubkey))) {
        cli_out(cli, "\r\nError: public key must be exactly %u hex digits"
                     " (compressed SEC1, as the device prints it)\r\n",
                (unsigned)(KS_PUBKEY_BYTES * 2u));
        return 0;
    }
    /* A prefix check, not validation: it catches a fingerprint pasted here.
     * radio_devices_docs/open_hub/cli.md */
    if (pubkey[0] != 0x02u && pubkey[0] != 0x03u) {
        cli_out(cli, "\r\nError: not a compressed point - first byte is %02x,"
                     " expected 02 or 03.\r\n"
                     "  the device prints the key above the fingerprint;"
                     " this argument is the key\r\n", pubkey[0]);
        return 0;
    }

    /* Says what re-enrolling would destroy; 'device window' is the reopen.
     * radio_devices_docs/open_hub/cli.md */
    {
        const ks_record_t *have = ks_find(dev_id);

        if (have != NULL && have->state == KS_STATE_PAIRED) {
            cli_out(cli, "\r\nError: 0x%08lx is paired in slot %u. 'device add'"
                         " would discard its session key.\r\n"
                         "  'device window %08lx' reopens the pairing window\r\n"
                         "  'device remove %08lx' first if you mean to re-enrol\r\n",
                    (unsigned long)dev_id, (unsigned)have->slot,
                    (unsigned long)dev_id, (unsigned long)dev_id);
            return 0;
        }
    }

    rc = ks_enrol(dev_id, pubkey, &slot);
    if (rc != 0) {
        const char *why = "flash write failed - see 'device list'";

        if (rc == -2) why = "device id 0 is not usable";
        else if (rc == -3) why = "no free uplink slot";
        cli_out(cli, "\r\nError: not enrolled: %s\r\n", why);
        return 0;
    }

    rc = rfm_request(IPC_REQ_ADD_DEVICE, 0, (const uint8_t *)&dev_id, sizeof(dev_id));
    if (rc == IPC_ST_OK)
        pairing_arm_init(dev_id, RADIO_PAIR_WINDOW_MS);
    if (rc < 0) {
        cli_out(cli, "\r\nenrolled in slot %u, but CM4 did not answer -"
                     " no pairing window is open\r\n", (unsigned)slot);
    } else if (rc != IPC_ST_OK) {
        cli_out(cli, "\r\nenrolled in slot %u, but CM4 refused the window,"
                     " status %d\r\n", (unsigned)slot, rc);
    } else {
        cli_out(cli, "\r\nenrolled 0x%08lx in slot %u, pairing window open\r\n",
                (unsigned long)dev_id, (unsigned)slot);
    }
    return 0;
}

static int cmd_device_remove(cli_data_t *cli, char **argv) {
    uint32_t dev_id = 0;

    if (parse_hex(argv[2], &dev_id)) {
        cli_out(cli, "\r\nError: device id must be hex\r\n");
        return 0;
    }
    if (ks_forget(dev_id) != 0) {
        cli_out(cli, "\r\nError: not enrolled, or flash write failed\r\n");
        return 0;
    }
    /* Nothing is sent to CM4: it holds no per-device state yet.
     * radio_devices_docs/open_hub/arch/ipc.md */
    cli_out(cli, "\r\nremoved 0x%08lx\r\n", (unsigned long)dev_id);
    return 0;
}

/* Newton on integers: the CLI has no float printf, and a sd is a sqrt.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static uint64_t isqrt64(uint64_t v) {
    uint64_t x, y;

    if (v < 2u)
        return v;
    x = v;
    y = (x + 1u) / 2u;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2u;
    }
    return x;
}

/* The second moment of the same edge cmd_device_synctime reports the range of. */
static int cmd_device_afcraw(cli_data_t *cli) {
    ipc_afc_raw_t r;
    unsigned i;
    int rc = rfm_request(IPC_REQ_GET_AFC_RAW, 0, NULL, 0);

    if (rc < 0 || rfm_reply.len < sizeof(r)) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    memcpy(&r, rfm_reply.payload, sizeof(r));

    /* Total against held: a full ring is a window, not the whole history. */
    cli_out(cli, "\r\nafc raw: %lu taken, %u held, newest first\r\n",
            (unsigned long)r.total, r.n);
    /* Level, gain and outcome beside the correction: one line is one frame. */
    for (i = 0; i < r.n; i++) {
        char level[16];

        /* A level taken after the frame ended is the floor, and says so. */
        if (((r.in_frame >> i) & 1u) != 0u)
            snprintf(level, sizeof(level), "%4d dBm", r.rssi[i]);
        else
            snprintf(level, sizeof(level), "   no lvl");
        char slot[12];

        /* The slot the edge landed in: how long the receiver had been in RX. */
        if (r.slot[i] == 0xFFu)
            snprintf(slot, sizeof(slot), "slot  --");
        else
            snprintf(slot, sizeof(slot), "slot %3u", r.slot[i]);
        cli_out(cli, "  grid %2u  %s  %7ld Hz  %s  lna G%u  %s\r\n",
                r.grid[i], slot, (long)IPC_AFC_STEPS_TO_HZ(r.afc[i]), level,
                r.gain[i], ((r.crc_ok >> i) & 1u) ? "ok" : "CRC FAIL");
    }
    /* The sampler's own record, so a column of "no lvl" names its cause. */
    cli_out(cli, "  levels: %u tried, %u late, %u failed, worst lag %u us\r\n",
            r.rssi_taken, r.rssi_late, r.rssi_err, r.lag_max_us);
    if (r.total > r.n)
        cli_out(cli, "  ... %lu older samples have been overwritten\r\n",
                (unsigned long)(r.total - r.n));
    return 0;
}

/* The hub half of the event deadline, in CM4's two terms. ROADMAP item 2
 * radio_devices_docs/open_hub/arch/ipc.md */
static int cmd_device_latency(cli_data_t *cli) {
    ipc_evt_latency_t l;
    int rc = rfm_request(IPC_REQ_GET_EVT_LAT, 0, NULL, 0);

    if (rc < 0 || rfm_reply.len < sizeof(l)) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    memcpy(&l, rfm_reply.payload, sizeof(l));

    cli_out(cli, "\r\nuplink events %lu sent, %lu timed, %lu unanswered\r\n",
            (unsigned long)l.sent, (unsigned long)l.replied,
            (unsigned long)l.lost);
    /* Not zero means a reply outlived its waiter, which the dispatch must not do. */
    if (l.stale != 0u)
        cli_out(cli, "*** %lu event reply(s) nobody was waiting for\r\n",
                (unsigned long)l.stale);
    if (l.replied == 0u) {
        cli_out(cli, "no reply has ever been timed - nothing below is a"
                     " measurement yet\r\n");
        return 0;
    }

    /* Named for what it spans: it carries CM4's poll granularity too. */
    cli_out(cli, "frame end to event sent: last %lu us, max %lu\r\n",
            (unsigned long)l.arrival_last_us, (unsigned long)l.arrival_max_us);
    if (l.arrival_bad != 0u)
        cli_out(cli, "*** %lu arrival(s) timed off an edge that was not theirs\r\n",
                (unsigned long)l.arrival_bad);
    /* CM7 cannot answer before it has handled, so the round trip is a ceiling. */
    cli_out(cli, "event sent to reply: last %lu us, min %lu, max %lu, mean %lu\r\n",
            (unsigned long)l.rtt_last_us, (unsigned long)l.rtt_min_us,
            (unsigned long)l.rtt_max_us,
            (unsigned long)(l.rtt_sum_us / l.replied));
    /* One number against one budget, and it is an upper bound on both terms. */
    cli_out(cli, "hub half at worst %lu us of the %lu us the deadline leaves\r\n",
            (unsigned long)(l.arrival_max_us + l.rtt_max_us),
            (unsigned long)RADIO_HUB_HANDLE_SLACK_US);
    return 0;
}

static int cmd_device_afc(cli_data_t *cli) {
    ipc_afc_t a;
    int64_t n, den, num, slope_milli, b_milli, mean;
    int rc = rfm_request(IPC_REQ_GET_AFC, 0, NULL, 0);

    if (rc < 0 || rfm_reply.len < sizeof(a)) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    memcpy(&a, rfm_reply.payload, sizeof(a));

    /* The count first: a correction of 0 Hz and no measurement print the same. */
    cli_out(cli, "\r\nafc: %lu frames measured, %lu reads failed\r\n",
            (unsigned long)a.n, (unsigned long)a.read_err);
    if (a.n == 0u) {
        cli_out(cli, "nothing measured yet - the value below would be invented\r\n");
        return 0;
    }
    n    = (int64_t)a.n;
    mean = a.sum_hz / n;
    cli_out(cli, "correction: last %ld Hz on grid %u, min %ld, max %ld, mean %ld\r\n",
            (long)a.last_hz, a.last_grid, (long)a.min_hz, (long)a.max_hz, (long)mean);

    /* Samples from one channel carry no slope, and a printed one would be noise. */
    den = n * a.sum_gg - a.sum_g * a.sum_g;
    if (den == 0) {
        cli_out(cli, "every sample on one channel - no slope against the grid\r\n");
        return 0;
    }
    num         = n * a.sum_gh - a.sum_g * a.sum_hz;
    slope_milli = num * 1000 / den;
    b_milli     = (a.sum_hz * 1000 - slope_milli * a.sum_g) / n;
    cli_out(cli, "on grid: slope %ld/1000 Hz per channel, at grid 0 %ld/1000 Hz\r\n",
            (long)slope_milli, (long)b_milli);

    /* A crossing is a division by the slope, so it needs one worth dividing by. */
    if (slope_milli >= 1000 || slope_milli <= -1000)
        cli_out(cli, "fit crosses zero at grid %ld/1000\r\n",
                (long)(-b_milli * 1000 / slope_milli));
    else
        cli_out(cli, "slope under 1 Hz per channel - no crossing worth printing\r\n");
    return 0;
}

static int cmd_device_syncstats(cli_data_t *cli) {
    ipc_syncstats_t s;
    int64_t n, var_d, var_l, cov, mean_l;
    int rc = rfm_request(IPC_REQ_GET_SYNCSTATS, 0, NULL, 0);

    if (rc < 0 || rfm_reply.len < sizeof(s)) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    memcpy(&s, rfm_reply.payload, sizeof(s));

    cli_out(cli, "\r\npaired edges %lu, unpaired %lu, beacons %lu\r\n",
            (unsigned long)s.n, (unsigned long)s.unpaired,
            (unsigned long)s.beacon_n);
    if (s.beacon_n != 0u)
        cli_out(cli, "beacon lead alone: min %lu us, max %lu, over every beacon\r\n",
                (unsigned long)s.beacon_min_us, (unsigned long)s.beacon_max_us);

    /* Two samples make a variance; one makes a number that looks like one. */
    if (s.n < 2u) {
        cli_out(cli, "fewer than two paired edges - no spread yet\r\n");
        return 0;
    }

    n     = (int64_t)s.n;
    var_d = ((int64_t)s.sumsq_d - s.sum_d * s.sum_d / n) / (n - 1);
    var_l = ((int64_t)s.lead_sumsq -
             (int64_t)s.lead_sum * (int64_t)s.lead_sum / n) / (n - 1);
    cov   = (s.cov_sum - s.sum_d * (int64_t)s.lead_sum / n) / (n - 1);
    mean_l = (int64_t)s.lead_sum / n;

    cli_out(cli, "arrival: mean %ld us, sd %lu, n %lu\r\n",
            (long)((int64_t)s.ref_us + s.sum_d / n),
            (unsigned long)isqrt64((uint64_t)(var_d < 0 ? 0 : var_d)),
            (unsigned long)s.n);
    cli_out(cli, "beacon lead, same n: mean %ld us, sd %lu\r\n",
            (long)mean_l,
            (unsigned long)isqrt64((uint64_t)(var_l < 0 ? 0 : var_l)));

    /* The regressor carries a trailing poll, so the slope is an upper bound.
     * radio_devices_docs/open_hub/radio/sync-timestamp.md */
    if (var_l > 0)
        cli_out(cli, "arrival on lead: slope %ld/1000, cov %ld\r\n",
                (long)(cov * 1000 / var_l), (long)cov);
    else
        cli_out(cli, "arrival on lead: the lead did not vary, no slope\r\n");
    return 0;
}

/* Where the sync word landed, and the ladder that says whether that is real.
 * radio_devices_docs/open_hub/radio/sync-timestamp.md */
static int cmd_device_synctime(cli_data_t *cli) {
    ipc_synctime_t s;
    int rc = rfm_request(IPC_REQ_GET_SYNCTIME, 0, NULL, 0);

    if (rc < 0 || rfm_reply.len < sizeof(s)) {
        cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        return 0;
    }
    memcpy(&s, rfm_reply.payload, sizeof(s));

    /* 0 >= 0 proves nothing: a ladder with no traffic has not run. */
    cli_out(cli, "\r\nsync edges %lu, frames accepted %lu -> %s\r\n",
            (unsigned long)s.edges, (unsigned long)s.frames,
            (s.edges < s.frames)
                ? "BROKEN: fewer edges than frames, DIO3 is not SyncAddressMatch"
            : (s.frames == 0u)
                ? "no evidence: no frame has arrived to compare against"
                : "ok");
    cli_out(cli, "RegDioMapping1 %02x read back, DIO3 asked %u\r\n",
            (unsigned)s.dio_map1, (unsigned)s.dio3_asked);

    if (s.lead_n != 0u)
        cli_out(cli, "tx command to first bit: last %lu us, min %lu, max %lu"
                     " over %lu frames\r\n",
                (unsigned long)s.lead_last_us, (unsigned long)s.lead_min_us,
                (unsigned long)s.lead_max_us, (unsigned long)s.lead_n);

    if (s.edges == 0u) {
        /* Never fired is not a working zero. */
        cli_out(cli, "no edge has ever been taken - nothing below is a"
                     " measurement yet\r\n");
        return 0;
    }

    cli_out(cli, "offset in superframe: last %lu us, min %lu, max %lu\r\n",
            (unsigned long)s.last_offset_us, (unsigned long)s.min_offset_us,
            (unsigned long)s.max_offset_us);
    cli_out(cli, "last at superframe %lu, %lu implausible\r\n",
            (unsigned long)s.last_superframe, (unsigned long)s.implausible);
    /* A moved offset and a moved clock are otherwise the same reading. */
    cli_out(cli, "last raw %lu tk at %+ld ppm\r\n",
            (unsigned long)s.last_offset_tk, (long)s.calib_ppm);
    return 0;
}

/* The 119 bytes the last exchange's confirmations were taken over.
 * radio_devices_docs/radio/pairing.md */
static int cmd_device_transcript(cli_data_t *cli) {
    uint32_t dev_id = 0, sf = 0;
    const uint8_t *t = pairing_last_transcript(&dev_id, &sf);

    if (t == NULL) {
        cli_out(cli, "\r\nno exchange has derived since boot\r\n");
        return 0;
    }

    cli_out(cli, "\r\ntranscript of the last derive, device 0x%08lx\r\n",
            (unsigned long)dev_id);
    /* The value that reached the transcript, and no other.
     * radio_devices_docs/open_hub/cli.md */
    cli_out(cli, "superframe fed in: %lu\r\n", (unsigned long)sf);

    static const struct { const char *name; uint8_t off, len; } f[] = {
        { "hub_id    ",  0,  4 }, { "dev_id    ",  4,  4 },
        { "superframe",  8,  4 }, { "dev_nonce ", 12,  8 },
        { "hub_pub   ", 20, 33 }, { "eph_pub   ", 53, 33 },
        { "dev_pub   ", 86, 33 },
    };
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        cli_out(cli, "  %s ", f[i].name);
        for (uint8_t b = 0; b < f[i].len; b++)
            cli_out(cli, "%02x", t[f[i].off + b]);
        cli_out(cli, "\r\n");
    }

    cli_out(cli, "flat 119:\r\n");
    for (int b = 0; b < 119; b++) {
        cli_out(cli, "%02x", t[b]);
        if ((b % 32) == 31) cli_out(cli, "\r\n");
    }
    cli_out(cli, "\r\n");
    return 0;
}

/* The hub's own long-term identity; devices hold the public half out of band. */
static int cmd_device_hubkey(cli_data_t *cli, int argc, char **argv) {
    uint8_t priv[32], pub[33];
    int rc;

    /* Recovered keys are shown, never written: the store cannot witness itself.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (argc == 3 && strcmp(argv[2], "recover") == 0) {
        uint8_t lpriv[32], lpub[33];

        if (ks_legacy_hub_key_get(lpriv) != 0) {
            cli_out(cli, "\r\nno hub key of an older format was found\r\n");
            return 0;
        }
        rc = crypto_p256_public(lpriv, lpub);
        memset(lpriv, 0, sizeof(lpriv));
        if (rc != 0) {
            cli_out(cli, "\r\nError: recovered a key but could not derive its"
                         " public point, rc=%d\r\n", rc);
            return 0;
        }
        cli_out(cli, "\r\nrecovered hub public key:\r\n  ");
        for (int b = 0; b < 33; b++)
            cli_out(cli, "%02X", lpub[b]);
        cli_out(cli, "\r\n\r\nCompare this against the hub key a paired device"
                     " holds.\r\nIf it matches: 'device hubkey commit'."
                     " If it does not, do NOT commit -\r\n"
                     "the shim read the wrong offset and committing would"
                     " overwrite\r\na record that is still readable.\r\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "commit") == 0) {
        rc = ks_legacy_commit();
        if (rc < 0)
            cli_out(cli, "\r\nError: nothing was written\r\n");
        else
            cli_out(cli, "\r\n%d record(s) migrated forward\r\n", rc);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "gen") == 0) {
        /* A key this build cannot read is not an absent key.
         * radio_devices_docs/open_hub/arch/keystore.md */
        if (ks_legacy_pending()) {
            cli_out(cli, "\r\nError: a hub key of an older format is present"
                         " and has not been recovered.\r\n  'device hubkey"
                         " recover' first - generating now would orphan every"
                         " paired device.\r\n");
            return 0;
        }
        if (ks_hub_key_get(priv) == 0) {
            memset(priv, 0, sizeof(priv));
            cli_out(cli, "\r\nError: a hub key already exists. Replacing it"
                         " orphans every paired device - each holds this hub's"
                         " public key and would stop trusting it.\r\n");
            return 0;
        }
        rc = crypto_p256_keygen(priv, pub);
        if (rc != 0) {
            cli_out(cli, "\r\nError: keygen failed, rc=%d\r\n", rc);
            return 0;
        }
        rc = ks_hub_key_set(priv);
        memset(priv, 0, sizeof(priv));
        if (rc != 0) {
            cli_out(cli, "\r\nError: generated but not stored, rc=%d -"
                         " nothing was kept\r\n", rc);
            return 0;
        }
        cli_out(cli, "\r\nhub key generated\r\n");
    } else if (argc != 2) {
        cli_out(cli, "\r\ndevice hubkey [gen]\r\n");
        return 0;
    }

    if (ks_hub_key_get(priv) != 0) {
        cli_out(cli, "\r\nno hub key yet - run 'device hubkey gen' once\r\n");
        return 0;
    }
    rc = crypto_p256_public(priv, pub);
    memset(priv, 0, sizeof(priv));
    if (rc != 0) {
        cli_out(cli, "\r\nError: stored key is unusable, rc=%d\r\n", rc);
        return 0;
    }

    /* Printed in full: this value is provisioned, not compared.
     * radio_devices_docs/open_hub/cli.md */
    cli_out(cli, "\r\nhub public key (compressed, provision this to devices):\r\n");
    for (int i = 0; i < 33; i++)
        cli_out(cli, "%02x", pub[i]);
    cli_out(cli, "\r\n");
    return 0;
}

static int cmd_device(cli_data_t *cli, int argc, char **argv) {
    /* every subcommand takes one hex argument and differs only by request type */
    static const struct {
        const char *name;
        uint8_t     type;
        uint8_t     in_payload;  /* 0 - pass in arg, 1 - pass in payload */
    } ops[] = {
        {"dump",   IPC_REQ_READ_REG,      0},
    };
    uint32_t value = 0;
    int rc;

    /* A window without a flash write, in a store that never erases. */
    if (strcmp(argv[1], "hop") == 0 && argc <= 3) {
        ipc_hop_at_t h;
        uint32_t sf = 0;

        /* Decimal, as a superframe counter is quoted everywhere else here.
         * radio_devices_docs/open_hub/cli.md */
        if (argc == 3)
            sf = (uint32_t)strtoul(argv[2], NULL, 0);
        if (rfm_request(IPC_REQ_HOP_AT, 0, (const uint8_t *)&sf,
                        (uint8_t)sizeof(sf)) != IPC_ST_OK ||
            rfm_reply.len < sizeof(h)) {
            cli_out(cli, "hop: no answer\r\n");
            return 0;
        }
        memcpy(&h, rfm_reply.payload, sizeof(h));
        /* The key on the channel's own line: otherwise it compares with nobody.
         * radio_devices_docs/open_hub/cli.md */
        cli_out(cli, "superframe %lu -> hop %u, grid slot %u, %lu Hz\r\n"
                     "  key %02x%02x%02x%02x... (%s)\r\n",
                (unsigned long)h.superframe, h.channel, h.grid_slot,
                (unsigned long)h.hz,
                h.key_head[0], h.key_head[1], h.key_head[2], h.key_head[3],
                h.placeholder ? "PLACEHOLDER, no device paired yet"
                              : "network key");
        cli_out(cli, "  count %u, deck:", h.count);
        for (unsigned b = 0; b < h.count && b < sizeof(h.deck); b++)
            cli_out(cli, " %u", h.deck[b]);
        cli_out(cli, "\r\n");
        return 0;
    }
    if (strcmp(argv[1], "window") == 0 && argc == 3) {
        uint32_t dev_id = 0;
        const ks_record_t *have;

        if (parse_hex(argv[2], &dev_id)) {
            cli_out(cli, "\r\nError: device id must be hex\r\n");
            return 0;
        }
        have = ks_find(dev_id);
        if (have == NULL || have->state == KS_STATE_DELETED) {
            cli_out(cli, "\r\nError: 0x%08lx is not enrolled\r\n",
                    (unsigned long)dev_id);
            return 0;
        }
        rc = rfm_request(IPC_REQ_ADD_DEVICE, 0, (const uint8_t *)&dev_id,
                         sizeof(dev_id));
        /* pair_v3's invitation rides the same window the join beacon does. */
        if (rc == IPC_ST_OK)
            pairing_arm_init(dev_id, RADIO_PAIR_WINDOW_MS);
        cli_out(cli, rc == IPC_ST_OK ? "\r\nwindow open for 0x%08lx\r\n"
                                     : "\r\nCM4 refused the window for 0x%08lx\r\n",
                (unsigned long)dev_id);
        return 0;
    }
    /* Reads the downlink nonce guard, sealing nothing. ROADMAP item 36 */
    if (strcmp(argv[1], "dlnonce") == 0 && argc == 2) {
        ipc_dl_nonce_probe_t pr;

        if (rfm_request(IPC_REQ_DL_NONCE_PROBE, 0, NULL, 0) != IPC_ST_OK ||
            rfm_reply.len < sizeof(pr)) {
            cli_out(cli, "\r\ndlnonce: no reply\r\n");
            return 0;
        }
        memcpy(&pr, rfm_reply.payload, sizeof(pr));
        if (!pr.used) {
            cli_out(cli, "\r\ndlnonce: nothing sealed yet, so nothing to read\r\n");
            return 0;
        }
        cli_out(cli, "\r\ndlnonce: dev 0x%08lX last sealed at sf %lu\r\n",
                (unsigned long)pr.dev_id, (unsigned long)pr.last_sf);
        /* The expected column is the test; a bare verdict would agree with itself. */
        cli_out(cli, "  same sf   %u (want 0)\r\n"
                     "  next sf   %u (want 1)\r\n"
                     "  prev sf   %u (want 0)\r\n  %s\r\n",
                pr.verdict_same, pr.verdict_next, pr.verdict_prev,
                (pr.verdict_same == 0u && pr.verdict_next == 1u &&
                 pr.verdict_prev == 0u) ? "guard reads both ways"
                                        : "GUARD IS WRONG");
        return 0;
    }
    if (strcmp(argv[1], "spiloop") == 0 && argc <= 3) {
        ipc_spi_loop_t r;
        uint32_t n = (argc == 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 200u;

        if (rfm_request(IPC_REQ_SPI_LOOP, 0, (const uint8_t *)&n, (uint8_t)sizeof(n)) != IPC_ST_OK ||
            rfm_reply.len < sizeof(r)) {
            cli_out(cli, "spiloop: no reply\r\n");
            return 0;
        }
        memcpy(&r, rfm_reply.payload, sizeof(r));
        cli_out(cli, "spiloop: SPI1 at %lu Hz\r\n", (unsigned long)r.spi_hz);
        cli_out(cli, "spiloop reg: %lu bad bytes (%lu of them bit 7), read %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                (unsigned long)r.reg_bad_bytes, (unsigned long)r.reg_xor_80,
                r.reg_read[0], r.reg_read[1], r.reg_read[2], r.reg_read[3],
                r.reg_read[4], r.reg_read[5], r.reg_read[6], r.reg_read[7]);
        cli_out(cli, "spiloop fifo: %lu passes, %lu bad (%lu bytes, %lu of them bit 7), %lu io err\r\n",
                (unsigned long)r.passes, (unsigned long)r.bad_passes,
                (unsigned long)r.bad_bytes, (unsigned long)r.xor_80,
                (unsigned long)r.io_err);
        if (r.bad_passes) {
            cli_out(cli, "  wrote:");
            for (int b = 0; b < 8; b++) cli_out(cli, " %02x", r.expect[b]);
            cli_out(cli, "\r\n  read: ");
            for (int b = 0; b < 8; b++) cli_out(cli, " %02x", r.first_bad[b]);
            cli_out(cli, "\r\n");
        }
        return 0;
    }
    if (strcmp(argv[1], "synctime") == 0 && argc == 2)
        return cmd_device_synctime(cli);
    if (strcmp(argv[1], "syncstats") == 0 && argc == 2)
        return cmd_device_syncstats(cli);
    if (strcmp(argv[1], "afc") == 0 && argc == 2)
        return cmd_device_afc(cli);
    if (strcmp(argv[1], "afcraw") == 0 && argc == 2)
        return cmd_device_afcraw(cli);
    if (strcmp(argv[1], "latency") == 0 && argc == 2)
        return cmd_device_latency(cli);
    /* Separates an overdriven front end from a transmitter that is not clean. */
    if (strcmp(argv[1], "lna") == 0 && argc == 3) {
        uint8_t sel = (uint8_t)atoi(argv[2]);

        if (sel > 6u) {
            cli_out(cli, "\r\nUsage: device lna <0=AGC | 1..6, each 6 dB down>\r\n");
            return 0;
        }
        if (rfm_request(IPC_REQ_SET_LNA, sel, NULL, 0) != IPC_ST_OK) {
            cli_out(cli, "\r\nError: CM4 refused it\r\n");
            return 0;
        }
        cli_out(cli, "\r\nLNA gain select %u%s\r\n", sel,
                sel == 0u ? " (AGC)" : "");
        return 0;
    }

    if (strcmp(argv[1], "transcript") == 0 && argc == 2)
        return cmd_device_transcript(cli);

    if (strcmp(argv[1], "pair") == 0 && argc == 2)
        return cmd_device_pair(cli);

    if (strcmp(argv[1], "store") == 0 && argc == 2)
        return cmd_device_store(cli);

    if (strcmp(argv[1], "list") == 0 && argc == 2)
        return cmd_device_list(cli);

    if (strcmp(argv[1], "hubkey") == 0)
        return cmd_device_hubkey(cli, argc, argv);

    /* Test scaffolding for a torn write. Reset afterwards to rescan.
     * radio_devices_docs/open_hub/arch/keystore.md */
    if (strcmp(argv[1], "torncounter") == 0 && argc == 2) {
        rc = rfm_request(IPC_REQ_KV_TORN, 0, NULL, 0);
        if (rc < 0)
            cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        else if (rc != IPC_ST_OK || rfm_reply.payload[0] == 0)
            cli_out(cli, "\r\nError: CM4 would not write it\r\n");
        else
            cli_out(cli, "\r\nbad record written to the counter log with a"
                         " winning seq; reset and check the hub still beacons\r\n");
        return 0;
    }

    if (strcmp(argv[1], "torn") == 0 && argc == 2) {
        if (ks_write_torn() != 0)
            cli_out(cli, "\r\nError: could not write a torn record\r\n");
        else
            cli_out(cli, "\r\ntorn record written with a winning seq;"
                         " reset and check it is ignored\r\n");
        return 0;
    }

    if (strcmp(argv[1], "add") == 0 && argc == 4)
        return cmd_device_add(cli, argv);

    if (strcmp(argv[1], "remove") == 0 && argc == 3)
        return cmd_device_remove(cli, argv);

    /* Forces a quiesce without a device, so an SDR can check the silence.
     * radio_devices_docs/open_hub/testing/sdr.md */
    if (strcmp(argv[1], "quiesce") == 0 && argc == 3) {
        if (parse_hex(argv[2], &value) || value == 0 || value > 255) {
            cli_out(cli, "\r\nError: quiesce expects 1..ff superframes\r\n");
            return 0;
        }
        rc = rfm_request(IPC_REQ_QUIESCE, (uint8_t)value, NULL, 0);
        if (rc < 0)
            cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        else if (rc != IPC_ST_OK)
            cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        else
            cli_out(cli, "\r\n%s\r\n",
                    rfm_reply.payload[0] ? "queued"
                                         : "refused: quiescing, or inside the minimum gap");
        return 0;
    }

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (strcmp(argv[1], ops[i].name) != 0 || argc != 3)
            continue;

        if (parse_hex(argv[2], &value)) {
            cli_out(cli, "\r\nError: %s expects a hex value\r\n", ops[i].name);
            return 0;
        }

        if (ops[i].in_payload)
            rc = rfm_request(ops[i].type, 0, (const uint8_t *)&value, sizeof(value));
        else
            rc = rfm_request(ops[i].type, (uint8_t)value, NULL, 0);

        if (rc < 0)
            cli_out(cli, "\r\nError: CM4 did not answer\r\n");
        else if (rc != IPC_ST_OK)
            cli_out(cli, "\r\nError: CM4 rejected it, status %d\r\n", rc);
        else if (ops[i].type == IPC_REQ_READ_REG)
            cli_out(cli, "\r\n0x%02x 0x%02x\r\n", (unsigned)value, rfm_reply.payload[0]);
        else
            cli_out(cli, "\r\nok\r\n");

        return 0;
    }

    cli_out(cli,
        "\r\ndevice <cmd> <args>\r\n"
        "add <id> <pubkey>       - enrol and open a pairing window\r\n"
        "hubkey recover|commit   - carry a hub key across a format change\r\n"
        "remove <id>             - forget a device\r\n"
        "dlnonce                 - read the downlink nonce guard, sealing nothing\r\n"
        "list                    - enrolled devices and their slots\r\n"
        "hubkey [gen]            - the hub's long-term identity\r\n"
        "pair                    - pairing state machine\r\n"
        "quiesce <n>             - suspend the grid for n superframes (test)\r\n"
        "store                   - durable superframe counter\r\n"
        "torn / torncounter      - inject a bad record (test)\r\n"
        "synctime                - where the sync word landed, as a range\r\n"
        "syncstats               - the same edge as a spread, and its regressor\r\n"
        "afc                     - carrier error per frame, and its slope on the grid\r\n"
        "afcraw                  - the individual corrections, newest first\r\n"
        "latency                 - the hub half of the event deadline\r\n"
        "lna <0..6>              - pin the front-end gain; 0 hands it back to AGC\r\n"
        "dump <reg>              - read an RFM69 register\r\n");
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


/* Why the link is down is one of several things, so it is printed, not inferred.
 * radio_devices_docs/open_hub/network/telemetry.md */
static const char *disc_reason_name(uint8_t r) {
    switch (r) {
    case OHT_DISC_REASON_NONE:           return "none";
    case OHT_DISC_REASON_CONNECT_FAILED: return "connect failed";
    case OHT_DISC_REASON_PEER_CLOSED:    return "server closed it";
    case OHT_DISC_REASON_SEND_FAILED:    return "send failed";
    case OHT_DISC_REASON_RECV_FAILED:    return "receive failed";
    case OHT_DISC_REASON_BAD_FRAME:      return "bad frame";
    case OHT_DISC_REASON_HELLO_REFUSED:  return "hello refused";
    case OHT_DISC_REASON_KEEPALIVE_LOST: return "silent too long";
    case OHT_DISC_REASON_OPERATOR:       return "turned off here";
    default:                             return "?";
    }
}

static const char *hello_reason_name(uint8_t r) {
    switch (r) {
    case OHT_ACK_OK:              return "accepted";
    case OHT_ACK_BAD_TOKEN:       return "bad token";
    case OHT_ACK_BAD_VERSION:     return "protocol version";
    case OHT_ACK_SCHEMA_MISMATCH: return "schema digest";
    case OHT_ACK_BUSY:            return "another hub is connected";
    default:                      return "?";
    }
}

static int cmd_telemetry(cli_data_t *cli, int argc, char **argv) {
    const telemetry_stats_t *s;
    const char *ip = NULL;
    uint16_t port = 0;

    if (argc >= 4 && strcmp(argv[1], "server") == 0) {
        int n = atoi(argv[2 + 1]);

        if (n <= 0 || n > 65535) {
            cli_out(cli, "\r\nError: port is 1..65535\r\n");
            return 0;
        }
        if (telemetry_configure(argv[2], (uint16_t)n, (argc >= 5) ? argv[4] : NULL) != 0) {
            cli_out(cli, "\r\nError: %s is not an IPv4 address\r\n", argv[2]);
            return 0;
        }
        telemetry_enable(1);
        /* Says configured, never connected: the task has not tried yet. */
        cli_out(cli, "\r\nserver %s:%d, link enabled - 'telem' shows whether it "
                     "connected\r\n", argv[2], n);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        telemetry_enable(1);
        cli_out(cli, "\r\nenabled\r\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        telemetry_enable(0);
        cli_out(cli, "\r\ndisabled\r\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "now") == 0) {
        telemetry_request_snapshot();
        cli_out(cli, "\r\nsnapshot requested\r\n");
        return 0;
    }
    if (argc != 1) {
        cli_out(cli, "\r\nUsage: telem [server <ip> <port> [token] | on | off | now]\r\n");
        return 0;
    }

    s = telemetry_get_stats(&ip, &port);
    cli_out(cli, "\r\nschema %s\r\n", OHT_SCHEMA_DIGEST);
    if (ip == NULL || ip[0] == 0)
        cli_out(cli, "no server set - 'telem server <ip> <port> [token]'\r\n");
    else
        cli_out(cli, "server %s:%u, %s\r\n", ip, port,
                s->enabled ? "enabled" : "disabled");
    cli_out(cli, "%s", s->connected ? "connected" : "down");
    if (s->connected)
        cli_out(cli, " for %lus, snapshot asked every %lums, achieved %lums\r\n",
                (unsigned long)(s->up_ms / 1000u), (unsigned long)s->snapshot_ms,
                (unsigned long)s->snapshot_gap_ms);
    else
        cli_out(cli, ", last reason: %s (%s)\r\n", disc_reason_name(s->last_disc_reason),
                hello_reason_name(s->hello_reason));
    cli_out(cli, "connects %lu, disconnects %lu, connect failures %lu\r\n",
            (unsigned long)s->connects, (unsigned long)s->disconnects,
            (unsigned long)s->connect_fail);
    cli_out(cli, "tx %lu frames / %lu bytes, %lu refused\r\n",
            (unsigned long)s->frames_tx, (unsigned long)s->bytes_tx,
            (unsigned long)s->tx_fail);
    cli_out(cli, "snapshots %lu, last cost %luus, %lu truncated\r\n",
            (unsigned long)s->snapshots, (unsigned long)s->snapshot_us,
            (unsigned long)s->snapshot_trunc);
    cli_out(cli, "events %lu pushed, %lu dropped\r\n",
            (unsigned long)s->events_pushed, (unsigned long)s->events_dropped);
    cli_out(cli, "commands %lu, %lu refused\r\n",
            (unsigned long)s->cmds_rx, (unsigned long)s->cmds_bad);
    return 0;
}

void CLI_Task(void *argument) {
    /* static: cliTask has a 2 KB stack and cli_data_t no longer fits comfortably */
    static cli_data_t cli;
    uint8_t c;

    UNUSED(argument);

    memset(&cli, 0, sizeof(cli));
    hub_ipc_init();
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
