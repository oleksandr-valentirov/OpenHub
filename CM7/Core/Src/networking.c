#include "cmsis_os.h"
#include "networking.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>


#define PING_IP_ADDR        "8.8.8.8"
#define PING_DATA_SIZE      32
#define PING_MAX_ATTEMPTS   4


struct user_server servers[USER_SERVERS_MAX_NUM];

void NetSwitchPHY(uint8_t eth_enabled) {
    UNUSED(eth_enabled);
}

uint8_t NetPingHost(const char *ip_addr) {

    return 0;
}
