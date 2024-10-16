#include "cmsis_os.h"
#include "networking.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* lwip includes */
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "lwip/sys.h"
#include "lwip/ip.h"

/* defines */
#define PING_TIMEOUT    2000
#define PING_INTERVAL   1000

struct ping_conn {
    struct netconn *conn;
    ip_addr_t target_ip;
    uint16_t seq_num;
};

/* variables */
struct user_server servers[USER_SERVERS_MAX_NUM];
extern struct netif gnetif;

void ping_recv(struct ping_conn *ping) {
    struct netbuf *buf;
    ip_addr_t addr;

    while (netconn_recv(ping->conn, &buf) == ERR_OK) {
        netbuf_copy(buf, (void *)&addr, sizeof(addr));

        struct icmp_echo_hdr *icmp_hdr = (struct icmp_echo_hdr *)buf->p->payload;
        if (icmp_hdr->type == ICMP_ER) {
            printf("Received ping response from %s: seq=%d\n", ipaddr_ntoa(&addr), ntohs(icmp_hdr->seqno));
            fflush(stdout);
        }
        netbuf_delete(buf);
    }
}

void ping_send(struct ping_conn *ping) {
    struct netbuf *buf;
    struct icmp_echo_hdr *icmp_hdr;
    uint16_t ping_size = sizeof(struct icmp_echo_hdr) + 8;

    buf = netbuf_new();
    if (buf) {
        netbuf_ref(buf, buf->p->payload, ping_size);
        icmp_hdr = (struct icmp_echo_hdr *)netbuf_alloc(buf, ping_size);
        if (icmp_hdr) {
            icmp_hdr->type = ICMP_ECHO;
            icmp_hdr->code = 0;
            icmp_hdr->seqno = htons(ping->seq_num++);
            icmp_hdr->id = htons(0x1234);
            icmp_hdr->chksum = inet_chksum(icmp_hdr, sizeof(struct icmp_echo_hdr));

            netconn_sendto(ping->conn, buf, &ping->target_ip, 0);
            netbuf_delete(buf);
        }
    }
}

void Ping_Task(void *argument) {
    UNUSED(argument);
    struct ping_conn ping;

    osDelay(pdMS_TO_TICKS(1000));

    if (ip4addr_aton("8.8.8.8", &(ping.target_ip)) == 0) {
        printf("bad IP for Ping_Task\r\n");
        fflush(stdout);
        vTaskDelete(NULL); 
    }
    ping.conn = netconn_new_with_proto_and_callback(NETCONN_RAW, IP_PROTO_ICMP, NULL);
    if (!ping.conn) {
        /* suspend task */
        printf("\r\nError: netconn_new_with_proto_and_callback\r\n");
        for (;;) {osDelay(1);}
    }
    if (netconn_bind(ping.conn, IP_ADDR_ANY, 0) != ERR_OK) {
        /* suspend task */
        printf("\r\nError: netconn_bind\r\n");
        for (;;) {osDelay(1);}
    }

    printf("Ping Task started\r\n");
    
    for(;;) {        
        ping_send(&ping);
        ping_recv(&ping);
        osDelay(pdMS_TO_TICKS(PING_INTERVAL));
    }
}
