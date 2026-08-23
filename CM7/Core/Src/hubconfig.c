/* One place that applies a setting and persists it, and replays them at boot.
 * radio_devices_docs/open_hub/arch/config-store.md */
#include "hubconfig.h"

#include <string.h>

#include "cfgstoreapi.h"
#include "telemetry.h"

#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

extern struct netif gnetif;   /* defined in lwip.c */

static cfgflash_err_t last_err = CFGF_OK;

/* Applied first, then written: a store that refuses must not silently undo it. */
static int persist(const cfg_config_t *c) {
    last_err = cfg_put_config(c);
    return (last_err == CFGF_OK) ? 0 : -2;
}

static void netif_static(uint32_t ip, uint32_t mask, uint32_t gw) {
    ip4_addr_t a, m, g;

    a.addr = ip;
    m.addr = mask;
    g.addr = gw;
    /* Released first, or the client keeps renewing on top of the static config. */
    LOCK_TCPIP_CORE();
    dhcp_release_and_stop(&gnetif);
    netif_set_addr(&gnetif, &a, &m, &g);
    UNLOCK_TCPIP_CORE();
}

void hubconfig_apply_boot(void) {
    const cfg_config_t *c = &cfg_image()->cfg;
    char dotted[16];
    ip4_addr_t a;

    if (c->ip_static && c->ip_addr != 0u)
        netif_static(c->ip_addr, c->ip_mask, c->ip_gw);

    /* Absent, not zero: 0.0.0.0 is not a server anybody configured. */
    if (c->telem_ip != 0u && c->telem_port != 0u) {
        a.addr = c->telem_ip;
        snprintf(dotted, sizeof(dotted), "%s", ip4addr_ntoa(&a));
        if (telemetry_configure(dotted, c->telem_port,
                                (c->telem_token[0] != 0) ? c->telem_token : NULL) == 0)
            telemetry_enable(1);
    }
}

int hubconfig_set_server(const char *ip, uint16_t port, const char *token) {
    cfg_config_t c = cfg_image()->cfg;
    ip4_addr_t parsed;

    if (ip == NULL || ip4addr_aton(ip, &parsed) == 0)
        return -1;
    if (telemetry_configure(ip, port, token) != 0)
        return -1;
    telemetry_enable(1);
    c.telem_ip = parsed.addr;
    c.telem_port = port;
    memset(c.telem_token, 0, sizeof(c.telem_token));
    if (token != NULL)
        snprintf(c.telem_token, sizeof(c.telem_token), "%s", token);
    return persist(&c);
}

int hubconfig_set_static(uint32_t ip, uint32_t mask, uint32_t gw) {
    cfg_config_t c = cfg_image()->cfg;

    netif_static(ip, mask, gw);
    c.ip_static = 1;
    c.ip_addr = ip;
    c.ip_mask = mask;
    c.ip_gw = gw;
    return persist(&c);
}

int hubconfig_set_dhcp(void) {
    cfg_config_t c = cfg_image()->cfg;

    LOCK_TCPIP_CORE();
    dhcp_start(&gnetif);
    UNLOCK_TCPIP_CORE();
    c.ip_static = 0;
    return persist(&c);
}

int hubconfig_set_link_lost(uint8_t misses) {
    cfg_config_t c = cfg_image()->cfg;

    /* Zero is the record's "never set", so it cannot also be a threshold. */
    if (misses == 0u)
        return -1;
    c.link_lost_misses = misses;
    return persist(&c);
}

uint8_t hubconfig_link_lost(void) {
    return cfg_link_lost_misses(&cfg_image()->cfg);
}

cfgflash_err_t hubconfig_last_err(void) {
    return last_err;
}
