/*
 * The USB network [WEB-NET-01..05]: TinyUSB's ECM/RNDIS function bridged to
 * lwIP, with a DHCP and a DNS server. After TinyUSB's net_lwip_webserver
 * example.
 */
#include "hal.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "dhserver.h"
#include "dnserver.h"
#include "lwip/init.h"
#include "board_identity.h"
#include "lwip/timeouts.h"
#include "lwip/ethip6.h"
#include "lwip/igmp.h"
#include "lwip/apps/mdns.h"
#include "http_server.h"
#include "net_txq.h"

#define INIT_IP4(a, b, c, d)                                                                                           \
    { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

static struct netif netif_data;
static struct pbuf *received_frame;

/* Nothing is sent until the host has had this long to configure the
 * interface after a mount or a resume. */
#define HOST_SETTLE_MS 500u
static volatile uint32_t mount_delay_until_ms = 0;

/* [WEB-API-13] For /api/net. */
volatile uint32_t net_rx_count;
volatile uint32_t net_rx_drop;
volatile uint32_t net_tx_fail;
volatile uint32_t net_tx_ok;
volatile uint32_t net_usb_events[4]; /* mounts, unmounts, suspends, resumes */

/* The device side's MAC, from board_identity (net_mac_init()); the value here
 * is a placeholder every board overwrites. lwIP's netif takes it with bit 0
 * of byte 5 flipped, so the two ends of the link differ. */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x00};
static char mdns_hostname[16];
static uint8_t mdns_suffix;

/* [WEB-NET-02] The third octet comes from the board's MAC, so each board is
 * its own /24: every board is a point-to-point link with its own DHCP
 * server, and two on one subnet hand the host the same lease twice.
 * net_mac_init() fills these in. */
static ip4_addr_t ipaddr = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t entries[] = {
    {{0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60},
};

static dhcp_config_t dhcp_config = {.router = INIT_IP4(0, 0, 0, 0),
                                    .port = 67,
                                    .dns = INIT_IP4(192, 168, 7, 1),
                                    "pyro",
                                    TU_ARRAY_SIZE(entries),
                                    entries};

/* Before tud_init() [DD-072]: the host reads the MAC from the ECM descriptor
 * once, at enumeration, and keeps the lease it is given; neither can be
 * changed afterwards. */
void net_mac_init(void) {
    memcpy(tud_network_mac_address, board_mac(), sizeof(tud_network_mac_address));

    const uint8_t n = board_subnet_octet();
    IP4_ADDR(&ipaddr, 192, 168, n, 1);
    IP4_ADDR(&entries[0].addr, 192, 168, n, 2);
    IP4_ADDR(&entries[1].addr, 192, 168, n, 3);
    IP4_ADDR(&dhcp_config.dns, 192, 168, n, 1);
}

/* [WEB-NET-05, DD-070] Link output never waits on the endpoint and never
 * drops a frame it could send a moment later: a busy endpoint holds the
 * frame (net_txq.h), and net_service() sends it as soon as the endpoint
 * frees, or once the host's settling time has passed. */
static net_txq_t txq;
volatile uint32_t net_tx_held;

static bool tx_ready(void) {
    return tud_ready();
}

static bool tx_can_send(void *frame) {
    if (mount_delay_until_ms != 0) {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - mount_delay_until_ms) < 0)
            return false;
        mount_delay_until_ms = 0;
    }
    return tud_network_can_xmit(((struct pbuf *)frame)->tot_len);
}

static void tx_send(void *frame) {
    tud_network_xmit(frame, 0);
    net_tx_ok++;
}

static void tx_hold(void *frame) {
    pbuf_ref((struct pbuf *)frame);
}

static void tx_release(void *frame) {
    pbuf_free((struct pbuf *)frame);
}

static const net_tx_ops_t tx_ops = {tx_ready, tx_can_send, tx_send, tx_hold, tx_release};

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void)netif;
    switch (net_tx_offer(&txq, &tx_ops, p)) {
    case NET_TX_SENT:
        return ERR_OK;
    case NET_TX_HELD:
        net_tx_held++;
        return ERR_OK;
    case NET_TX_FULL:
        net_tx_fail++;
        return ERR_MEM; /* TCP's retransmission resends it */
    default:
        net_tx_fail++;
        return ERR_USE;
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
    if (received_frame) {
        net_rx_drop++;
        lwip_uart_printf("!NET rx drop (slot busy) sz=%u\r\n", size);
        return false;
    }
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            memcpy(p->payload, src, size);
            received_frame = p;
            net_rx_count++;
            if ((net_rx_count % 10) == 0) {
                lwip_uart_printf("!NET rx ok cnt=%lu\r\n", (unsigned long)net_rx_count);
            }
        } else {
            net_rx_drop++;
            lwip_uart_printf("!NET rx drop (no pbuf) sz=%u\r\n", size);
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
    lwip_uart_printf("!NET init_cb\r\n");
    net_tx_flush(&txq, &tx_ops);
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

/* ── TinyUSB's device lifecycle, counted for /api/net ─────────────── */

void tud_mount_cb(void) {
    net_usb_events[0]++;
    lwip_uart_printf("!USB mount\r\n");
    mount_delay_until_ms = to_ms_since_boot(get_absolute_time()) + HOST_SETTLE_MS;
}

void tud_umount_cb(void) {
    net_usb_events[1]++;
    lwip_uart_printf("!USB unmount\r\n");
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    net_usb_events[2]++;
    lwip_uart_printf("!USB suspend\r\n");
}

void tud_resume_cb(void) {
    net_usb_events[3]++;
    lwip_uart_printf("!USB resume\r\n");
    mount_delay_until_ms = to_ms_since_boot(get_absolute_time()) + HOST_SETTLE_MS;
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
    while (!netif_is_up(&netif_data)) {
    }
    dhserv_init(&dhcp_config);
    dnserv_init(IP_ADDR_ANY, 53, dns_query_proc);
}

/* [WEB-NET-03, WEB-NET-04] Starts mDNS on the first call. */
void net_mdns_poll(void) {
    if (mdns_started)
        return;
    mdns_started = true;
    snprintf(mdns_hostname, sizeof(mdns_hostname), "pyro");
    mdns_suffix = 0;
    mdns_resp_init();
    mdns_resp_register_name_result_cb(mdns_name_result);
    mdns_resp_add_netif(&netif_data, mdns_hostname);
    mdns_resp_add_service(&netif_data, mdns_hostname, "_pyro", DNSSD_PROTO_TCP, 80, NULL, NULL);
}

/* How long the last call's HTTP transport took, for STAGE 1's breakdown. */
uint32_t net_last_http_us;

void net_service(void) {
    net_tx_drain(&txq, &tx_ops);
    if (received_frame) {
        if (ethernet_input(received_frame, &netif_data) != ERR_OK)
            pbuf_free(received_frame);
        received_frame = NULL;
        tud_network_recv_renew();
    }
    sys_check_timeouts();
    /* Outside every lwIP callback: the bytes the callbacks queued. */
    uint32_t t0 = time_us_32();
    http_server_transport();
    net_tx_drain(&txq, &tx_ops);
    net_last_http_us = time_us_32() - t0;
}

/* lwIP system hooks */
sys_prot_t sys_arch_protect(void) {
    return 0;
}
void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}
uint32_t sys_now(void) {
    return to_ms_since_boot(get_absolute_time());
}
unsigned int lwip_port_rand(void) {
    return to_ms_since_boot(get_absolute_time());
}

#include <stdarg.h>
void lwip_uart_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        hal_telemetry_send(buf);
}
