#include "cmsis_os.h"
#include "networking.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "hsem_table.h"

/* lwip includes */
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "lwip/inet.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"
#include "lwip/inet_chksum.h"

/* defines */
#define PING_TIMEOUT    2000
#define PING_ID         0xABCD
#define PING_DATA_SIZE  32

/* structures */
typedef struct ping_args {
    const char *ip;
    ip4_addr_t target_ip;
    struct raw_pcb *pcb;
    struct pbuf *p;
    uint16_t id;
    uint8_t ping_resp_flag;
} ping_args_t;

/* variables */
struct user_server servers[USER_SERVERS_MAX_NUM];
extern struct netif gnetif;

static uint8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    UNUSED(pcb);
    UNUSED(addr);
    struct icmp_echo_hdr *icmp_hdr;
    ping_args_t *args = (ping_args_t *)arg;

    if ((p->tot_len >= (PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))) && pbuf_remove_header(p, PBUF_IP_HLEN) == 0) {
        icmp_hdr = (struct icmp_echo_hdr *)p->payload;
        if ((icmp_hdr->type == ICMP_ER) && (icmp_hdr->id == args->id)) {
            args->ping_resp_flag = 1;
            pbuf_free(p);
            return 1;
        }
        pbuf_add_header(p, PBUF_IP_HLEN);
    }

    return 0;
}

static void ping_send(ping_args_t *args, uint16_t ping_seq_num) {
    struct icmp_echo_hdr *icmp_hdr;
    uint16_t icmp_hdr_size = sizeof(struct icmp_echo_hdr);

    icmp_hdr = (struct icmp_echo_hdr *)(args->p->payload);
    icmp_hdr->type = ICMP_ECHO;
    icmp_hdr->code = 0;
    icmp_hdr->seqno = lwip_htons(ping_seq_num);

    /* generate random ID or use default */
    if (args->id == 0) {
        while (HAL_HSEM_IsSemTaken(HSEM_RNG) && !LL_RNG_IsActiveFlag_DRDY(RNG)) {}
        HAL_HSEM_FastTake(HSEM_RNG);
        icmp_hdr->id = (uint16_t)LL_RNG_ReadRandData32(RNG);
        HAL_HSEM_Release(HSEM_RNG, 0);
    } else
        icmp_hdr->id = args->id;

    /* store ID to compare with response ID in a callback */
    args->id = icmp_hdr->id;

    memset((u8_t *)icmp_hdr + icmp_hdr_size, 0xAB, PING_DATA_SIZE);

    icmp_hdr->chksum = 0;
    icmp_hdr->chksum = inet_chksum(icmp_hdr, icmp_hdr_size + PING_DATA_SIZE);

    raw_sendto(args->pcb, args->p, &(args->target_ip));
}


void Networking_ping_command(const char *ip_addr, uint8_t repeats, uint8_t break_on_response, uint8_t use_stdout, void (*onSuccessCallback)(void)) {
    ping_args_t ping_args;
    ping_args.pcb = raw_new(IP_PROTO_ICMP);
    ping_args.ip = ip_addr;
    ping_args.ping_resp_flag = 0;
    ping_args.id = 0;
    uint32_t start = 0;

    printf("\r\n");
    ping_args.p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE, PBUF_RAM);
    if (ping_args.p == NULL) {
        printf("Failed to allocate pbuf for ping request.\r\n");
        return;
    }

    if (ping_args.pcb == NULL) {
        printf("Failed to create RAW socket for ping.\r\n");
        return;
    }

    if (ip4addr_aton(ip_addr, &ping_args.target_ip) == 0) {
        printf("Bad IP for Ping_Task\r\n");
        return;
    }

    raw_recv(ping_args.pcb, ping_recv, &ping_args); /* set callback to process response */
    raw_bind(ping_args.pcb, IP_ADDR_ANY);

    for(uint8_t i = 0; i < repeats; i++) {
        if (use_stdout)
            printf("Ping request sent to %s\r\n", ip_addr);

        ping_send(&ping_args, i + 1);
        start = xTaskGetTickCount();  /* wait till either the response is ready or the timeout */
        while (!ping_args.ping_resp_flag && (xTaskGetTickCount() < (start + 1000)))
            osDelay(1);

        if (ping_args.ping_resp_flag) {
            if (use_stdout)
                printf("Ping reply received from %s, seq: %d\r\n", ip_addr, i + 1);

            if (onSuccessCallback)
                onSuccessCallback();

            if (break_on_response)
                break;
            ping_args.ping_resp_flag = 0;
        } else if (use_stdout)
            printf("Timeout\r\n");
    }

    pbuf_free(ping_args.p);
    raw_remove(ping_args.pcb);
}

/*
 * it is implemented with temp variable and memcpy
 * because ip4addr_ntoa has static local variable which gets overwritten
 * with the latest argument passted to the function
 * if a ip4addr_ntoa call placed inside of the sprintf multiple times.
 */
int Networking_get_network_info(char *resp_buffer) {
    char ip_addr[16] = {0};
    char netmask[16] = {0};
    char gw[16] = {0};
    char *temp = NULL;

    /* copy ip addr */
    temp = ip4addr_ntoa(&gnetif.ip_addr);
    memcpy(ip_addr, temp, strlen(temp));
    /* copy netmask */
    temp = ip4addr_ntoa(&gnetif.netmask);
    memcpy(netmask, temp, strlen(temp));
    /* copy gateway */
    temp = ip4addr_ntoa(&gnetif.gw);
    memcpy(gw, temp, strlen(temp));

    return sprintf(resp_buffer, 
                   "\r\nIP addr: %s\r\nNetmask: %s\r\nGateway: %s\r\nMAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n", 
                   ip_addr, netmask, gw,
                   gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
                   gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);
}
