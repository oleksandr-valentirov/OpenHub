#pragma once

#include <stdint.h>
#include "main.h"

/**
 * @file networking.h
 * @brief lwIP addressing and the ping command, on CM7 because the MAC is.
 *
 * radio_devices_docs/open_hub/network/ethernet.md
 */

/**
 * @brief Renders the interface's addressing into a caller's buffer.
 * @param resp_buffer  receives the text
 * @return bytes written
 *
 * Calls into lwIP under LOCK_TCPIP_CORE() because the CLI runs outside tcpip_thread.
 */
int Networking_get_network_info(char *resp_buffer);

/**
 * @brief Sends ICMP echoes and reports what came back.
 * @param ip_addr            the target, as text
 * @param repeats            how many echoes to send
 * @param break_on_response  stop at the first reply rather than sending them all
 * @param use_stdout         write progress to the console as it goes
 * @param onSuccessCallback  called once if any reply arrived, or NULL
 */
void Networking_ping_command(const char *ip_addr, uint8_t repeats, uint8_t break_on_response,
                             uint8_t use_stdout, void (*onSuccessCallback)(void));
