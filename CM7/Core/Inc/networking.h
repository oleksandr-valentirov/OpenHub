#pragma once

#include <stdint.h>
#include "main.h"
#include "ip4_addr.h"

#define USER_SERVERS_MAX_NUM  2
#define USER_SERVER_NAME_LEN  32

typedef struct user_server {
  char name[USER_SERVER_NAME_LEN];
  int sock;
  ip4_addr_t ip;
  uint8_t status;  /* 0 - not active, !0 - active */
} user_server_t;

extern struct user_server servers[USER_SERVERS_MAX_NUM];

int Networking_get_network_info(char *resp_buffer);
void Networking_ping_command(const char *ip_addr, uint8_t repeats, uint8_t break_on_response, uint8_t use_stdout, void (*onSuccessCallback)(void));
