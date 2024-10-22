#include "cmsis_os.h"
#include "networking.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

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

/* variables */
struct user_server servers[USER_SERVERS_MAX_NUM];
extern struct netif gnetif;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    struct icmp_echo_hdr *icmp_hdr;

    if (p->len >= (sizeof(struct icmp_echo_hdr))) {
        icmp_hdr = (struct icmp_echo_hdr *)p->payload;
        if ((icmp_hdr->type == ICMP_ER) && (icmp_hdr->id == PING_ID)) {
            printf("Ping reply received from %s, seq: %d\n", ipaddr_ntoa(addr), lwip_ntohs(icmp_hdr->seqno));
            pbuf_free(p);
            return 1;
        }
    }
    pbuf_free(p);
    return 0;
}

static void ping_send(ip4_addr_t *target_ip, struct raw_pcb *pcb) {
    struct pbuf *p;
    struct icmp_echo_hdr *icmp_hdr;
    u16_t icmp_hdr_size = sizeof(struct icmp_echo_hdr);
    u16_t ping_seq_num = 0;

    /* Створюємо ICMP пакет */
    p = pbuf_alloc(PBUF_IP, icmp_hdr_size + PING_DATA_SIZE, PBUF_RAM);
    if (p == NULL) {
        printf("\r\nFailed to allocate pbuf for ping request.\r\n");
        return;
    }

    icmp_hdr = (struct icmp_echo_hdr *)p->payload;
    icmp_hdr->type = ICMP_ECHO; // 8 - Echo request
    icmp_hdr->code = 0;
    icmp_hdr->id = PING_ID;
    icmp_hdr->seqno = lwip_htons(++ping_seq_num);
    memset((u8_t *)icmp_hdr + icmp_hdr_size, 0xAB, PING_DATA_SIZE);

    icmp_hdr->chksum = 0;
    icmp_hdr->chksum = inet_chksum(icmp_hdr, icmp_hdr_size + PING_DATA_SIZE);

    raw_sendto(pcb, p, target_ip);

    printf("\r\nPing request sent to %s\r\n", ipaddr_ntoa(target_ip));

    pbuf_free(p);
}


void Ping_Task(void *argument) {
    ip4_addr_t target_ip;
    struct raw_pcb *ping_pcb = raw_new(IP_PROTO_ICMP);

    osDelay(5000);

    if (ping_pcb == NULL) {
        printf("Failed to create RAW socket for ping.\n");
        vTaskDelete(NULL);
    }

    if (ip4addr_aton("8.8.8.8", &target_ip) == 0) {
        printf("bad IP for Ping_Task\r\n");
        fflush(stdout);
        vTaskDelete(NULL); 
    }

    raw_recv(ping_pcb, ping_recv, NULL); /* set callback to process response */
    raw_bind(ping_pcb, IP_ADDR_ANY);

    for(;;) {
        osDelay(1000);
        ping_send(&target_ip, ping_pcb);
    }
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
