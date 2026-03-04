/*
 * USB network glue: TinyUSB ECM/RNDIS ↔ lwIP bridge + DHCP server.
 * Adapted from TinyUSB net_lwip_webserver example.
 */
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "dhserver.h"
#include "dnserver.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/ethip6.h"
#include "lwip/igmp.h"
#include "lwip/apps/mdns.h"

#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

static struct netif netif_data;
static struct pbuf *received_frame;

/* Derived from board unique ID at init time */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};
static char mdns_hostname[16];
static uint8_t mdns_suffix;

static const ip4_addr_t ipaddr  = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t entries[] = {
    {{0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config = {
    .router = INIT_IP4(0, 0, 0, 0),
    .port = 67,
    .dns = INIT_IP4(192, 168, 7, 1),
    "pyro",
    TU_ARRAY_SIZE(entries),
    entries
};

/* Must be called BEFORE tud_init() — currently a no-op with hardcoded MAC */
void net_mac_init(void) {
}

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void)netif;
    for (;;) {
        if (!tud_ready()) return ERR_USE;
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        tud_task();
    }
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif) {
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP | NETIF_FLAG_IGMP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->linkoutput = linkoutput_fn;
    netif->output = ip4_output_fn;
    return ERR_OK;
}

bool dns_query_proc(const char *name, ip4_addr_t *addr) {
    if (0 == strcmp(name, "pyro.local")) {
        *addr = ipaddr;
        return true;
    }
    return false;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (received_frame) return false;
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            memcpy(p->payload, src, size);
            received_frame = p;
        }
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

void net_init(void) {
    struct netif *netif = &netif_data;
    lwip_init();

    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    netif->hwaddr[5] ^= 0x01;

    netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ip_input);
    netif_set_default(netif);
}

static void mdns_name_result(struct netif *netif, u8_t result, s8_t slot) {
    (void)slot;
    if (result == MDNS_PROBING_CONFLICT) {
        mdns_suffix++;
        snprintf(mdns_hostname, sizeof(mdns_hostname), "pyro-%u", mdns_suffix);
        mdns_resp_rename_netif(netif, mdns_hostname);
    }
}

static bool mdns_started = false;

void net_start(void) {
    while (!netif_is_up(&netif_data)) {}
    dhserv_init(&dhcp_config);
    dnserv_init(IP_ADDR_ANY, 53, dns_query_proc);
}

/* Call from main loop — starts mDNS once, safe to call repeatedly */
void net_mdns_poll(void) {
    if (mdns_started) return;
    mdns_started = true;
    snprintf(mdns_hostname, sizeof(mdns_hostname), "pyro");
    mdns_suffix = 0;
    mdns_resp_init();
    mdns_resp_register_name_result_cb(mdns_name_result);
    mdns_resp_add_netif(&netif_data, mdns_hostname);
    mdns_resp_add_service(&netif_data, mdns_hostname, "_pyro",
                          DNSSD_PROTO_TCP, 80, NULL, NULL);
}

void net_service(void) {
    if (received_frame) {
        if (ethernet_input(received_frame, &netif_data) != ERR_OK)
            pbuf_free(received_frame);
        received_frame = NULL;
        tud_network_recv_renew();
    }
    sys_check_timeouts();
}

/* lwIP system hooks */
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
uint32_t sys_now(void) { return to_ms_since_boot(get_absolute_time()); }
unsigned int lwip_port_rand(void) { return to_ms_since_boot(get_absolute_time()); }
