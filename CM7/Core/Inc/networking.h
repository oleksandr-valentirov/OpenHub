#pragma once

#include <stdint.h>
#include "main.h"

#define USER_SERVERS_MAX_NUM  2
#define USER_SERVER_NAME_LEN  32

typedef struct user_server {
  char name[USER_SERVER_NAME_LEN];
  int sock;
  uint8_t status;  /* 0 - not active, !0 - active */
} user_server_t;

void NetSwitchPHY(uint8_t eth_enabled);
void Ping_Task(void *argument);
