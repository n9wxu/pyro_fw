/*
 * USB network glue: TinyUSB ECM/RNDIS ↔ lwIP bridge + DHCP server.
 * Adapted from TinyUSB net_lwip_webserver example.
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
#include "lwip/prot/ethernet.h"
#include "hardware/structs/rosc.h"
#include "http_server.h"
#include "mac_random.h"
#include "net_txq.h"

#define INIT_IP4(a, b, c, d)                                                                                           \
    { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

static struct netif netif_data;
static struct pbuf *received_frame;

/* USB mount delay - give host time to configure ECM interface */
static volatile uint32_t mount_delay_until_ms = 0;

/* ── Network diagnostic counters (read by main_hardware.c) ────────── */
volatile uint32_t net_rx_count;
volatile uint32_t net_rx_drop;
volatile uint32_t net_tx_fail;
volatile uint32_t net_tx_ok;
volatile uint32_t net_usb_events[4]; /* mounts, unmounts, suspends, resumes: /api/net */

/* Device-side MAC. Filled by net_mac_init() from board_identity, which must
 * run before tud_init() because the ECM descriptor carries this as a string
 * and the host reads it once, at enumeration.
 *
 * The compiled-in value is a placeholder that every board overwrites. Boards
 * sharing one MAC leave a host with one usable board: the rest enumerate and
 * are ignored.
 *
 * lwIP's netif takes the same address with bit 0 of byte 5 flipped, so the
 * two ends of the link differ. */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x00};
static char mdns_hostname[16];
static uint8_t mdns_suffix;

/* The third octet comes from the board's MAC, so each board is its own /24.
 * They cannot share one: every board is a point-to-point USB link running its
 * own DHCP server, so two boards on 192.168.7.0/24 hand the host the same
 * lease twice and the host can only route to one of them.
 *
 * Not const, and not compile-time: net_mac_init() fills them in. */
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

/* MUST be called before tud_init(): the MAC goes into the ECM descriptor,
 * which the host reads once at enumeration, and the addresses go into the
 * DHCP server, which hands out a lease the host will not renegotiate.
 * Re-addressing afterwards does not work -- there is no shared segment to
 * announce on and no way to force the host to renew. */
void net_mac_init(void) {
    memcpy(tud_network_mac_address, board_mac(), sizeof(tud_network_mac_address));

    const uint8_t n = board_subnet_octet();
    IP4_ADDR(&ipaddr, 192, 168, n, 1);
    IP4_ADDR(&entries[0].addr, 192, 168, n, 2);
    IP4_ADDR(&entries[1].addr, 192, 168, n, 3);
    IP4_ADDR(&dhcp_config.dns, 192, 168, n, 1);
}

/* Link output never waits on the endpoint and never drops a frame it could
 * send a moment later: a busy endpoint holds the frame (net_txq.h), and
 * net_service() sends it as soon as the endpoint frees [G4-N]. Frames held
 * through the host's settling time after a mount go out once it has passed. */
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
    if (net_tx_copy_len(p->tot_len, CFG_TUD_NET_MTU) == 0) {
        net_tx_fail++;
        return ERR_BUF;
    }
    switch (net_tx_offer(&txq, &tx_ops, p)) {
    case NET_TX_SENT:
        return ERR_OK;
    case NET_TX_HELD:
        net_tx_held++;
        return ERR_OK;
    case NET_TX_FULL:
        net_tx_fail++;
        return ERR_MEM; /* lwIP's retransmit has it */
    default:
        net_tx_fail++;
        return ERR_USE;
    }
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

/* CFG_TUD_NET_MTU is the endpoint buffer, a whole Ethernet frame; lwIP's MTU
 * is the IP packet inside it. */
_Static_assert(CFG_TUD_NET_MTU - SIZEOF_ETH_HDR == 1500, "an IP MTU of 1500, as TCP_MSS assumes (lwipopts.h)");

static err_t netif_init_cb(struct netif *netif) {
    netif->mtu = CFG_TUD_NET_MTU - SIZEOF_ETH_HDR;
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

static void *rx_alloc(uint16_t size) {
    return pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
}

static bool rx_fill(void *frame, const uint8_t *src, uint16_t size) {
    return pbuf_take((struct pbuf *)frame, src, size) == ERR_OK;
}

static const net_rx_ops_t rx_ops = {rx_alloc, rx_fill, tx_release};

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    bool busy = received_frame != NULL;
    if (!net_rx_take((void **)&received_frame, &rx_ops, src, size)) {
        net_rx_drop++;
        lwip_uart_printf("!NET rx drop (%s) sz=%u\r\n", busy ? "slot busy" : "no pbuf", size);
        return false;
    }
    net_rx_count++;
    if ((net_rx_count % 10) == 0) {
        lwip_uart_printf("!NET rx ok cnt=%lu\r\n", (unsigned long)net_rx_count);
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    return pbuf_copy_partial(p, dst, net_tx_copy_len(p->tot_len, CFG_TUD_NET_MTU), 0);
}

void tud_network_init_cb(void) {
    lwip_uart_printf("!NET init_cb\r\n");
    net_tx_flush(&txq, &tx_ops);
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

/* ── TinyUSB device lifecycle callbacks (instrumentation) ────────────── */

void tud_mount_cb(void) {
    net_usb_events[0]++;
    lwip_uart_printf("!USB mount\r\n");
    /* Give host 500ms to configure ECM interface before sending packets */
    mount_delay_until_ms = to_ms_since_boot(get_absolute_time()) + 500;
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
    /* Give host time to reconfigure ECM interface after resume */
    mount_delay_until_ms = to_ms_since_boot(get_absolute_time()) + 500;
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

/* Call from main loop — starts mDNS once, safe to call repeatedly */
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
    /* Process received frames - RX always works */
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
/* TCP's initial sequence numbers and mDNS's probe delays: they must differ
 * from boot to boot, not be secret. The ring oscillator's random bit
 * (rp2040-datasheet_2025-02-20.pdf, section 2.17.5, page 223) and the timer,
 * mixed [DD-072]. */
unsigned int lwip_port_rand(void) {
    static uint64_t pool;
    for (int i = 0; i < 32; i++) {
        pool = (pool << 1 | pool >> 63) ^ (rosc_hw->randombit & 1u);
    }
    pool += time_us_64();
    return (unsigned int)mac_mix64(pool);
}

#include <stdarg.h>
void lwip_uart_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        hal_telemetry_send(buf); /* v2-11: non-blocking DMA path */
}
