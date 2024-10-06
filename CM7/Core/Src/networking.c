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
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/inet_chksum.h"

/* defines */
#define PING_TIMEOUT    2000

/* variables */
struct user_server servers[USER_SERVERS_MAX_NUM];
extern struct netif gnetif;


void Ping_Task(void *argument) {
    ip4_addr_t target_ip, source_ip;
    // err_t err = ERR_OK;
    struct icmp_echo_hdr *icmp_hdr = NULL;
    struct pbuf *p = NULL;

    if (ip4addr_aton((char *)argument, &target_ip) == 0) {
        printf("bad IP for Ping_Task\r\n");
        fflush(stdout);
        vTaskDelete(NULL); 
    }
    
    for(;;) {
        p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
        if (p != NULL) {
            icmp_hdr = (struct icmp_echo_hdr *)p->payload;
            icmp_hdr->type = ICMP_ECHO; 
            icmp_hdr->code = 0;
            icmp_hdr->chksum = 0; 
            icmp_hdr->id = htons(12345); 
            icmp_hdr->seqno = htons(1); 
            icmp_hdr->chksum = inet_chksum(icmp_hdr, sizeof(struct icmp_echo_hdr));

            // if ((err = ip_send(&gnetif, &target_ip, &source_ip, p)) != ERR_OK) {
            //     // printf("Failed to send ping request\r\n");
            // }

            vTaskDelay(pdMS_TO_TICKS(PING_TIMEOUT));
            pbuf_free(p);
        } else {
            // printf("Failed to allocate pbuf\r\n");
        }

        osDelay(pdMS_TO_TICKS(2000));
    }
}
