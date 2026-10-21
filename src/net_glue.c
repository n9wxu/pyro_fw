/*
 * The USB network (net_glue.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_glue.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/structs/rosc.h"
#include "board_identity.h"
#include "http_server.h"
#include "http_transport.h"
#include "net_stcp.h"
#include "net_txq.h"
#include "eth.h"
#include "udp.h"
#include "dhcpv4_server.h"
#include "mdns.h"
#include <stdio.h>
#include <string.h>

#define FRAME_MAX CFG_TUD_NET_MTU
_Static_assert(FRAME_MAX == 1514, "a whole Ethernet frame: an IP MTU of 1500");

static net_t net;
/* Never written: frames reach the stack in TinyUSB's own buffer. net_init()
 * wants one all the same, and takes the receive MSS from its size
 * (smallest_tcp issue 11). */
static uint8_t rx_frame[FRAME_MAX];
static uint8_t tx_frame[FRAME_MAX];
static net_rx_slot_t rx_slot;
static net_link_counts_t counts;

/* Device-side MAC, from board_identity. Boards sharing one leave a host with
 * one usable board: the rest enumerate and are ignored. The stack takes the
 * same address with bit 0 of byte 5 flipped, so the two ends differ. */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x00};

/* ── Transmit [WEB-NET-05] ────────────────────────────────────────── */

#define HOST_SETTLE_MS 500u /* after a mount or a resume, before the first frame */

static net_txq_t txq;
static uint32_t settle_until_ms; /* 0: the host has had its time */

static bool host_settled(void) {
    if (settle_until_ms != 0) {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - settle_until_ms) < 0)
            return false;
        settle_until_ms = 0;
    }
    return true;
}

static bool ep_ready(void) {
    return tud_ready();
}

static bool ep_can_send(uint16_t len) {
    return host_settled() && tud_network_can_xmit(len);
}

static void ep_send(const uint8_t *frame, uint16_t len) {
    tud_network_xmit((void *)(uintptr_t)frame, len);
    counts.tx_sent++;
}

static const net_tx_ops_t ep_ops = {ep_ready, ep_can_send, ep_send};

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    uint16_t n = net_tx_copy_len(arg, FRAME_MAX);
    memcpy(dst, ref, n);
    return n;
}

static int mac_send(void *ctx, const uint8_t *frame, uint16_t len) {
    (void)ctx;
    switch (net_tx_offer(&txq, &ep_ops, frame, len)) {
    case NET_TX_SENT:
        return len;
    case NET_TX_HELD:
        counts.tx_held++;
        return len;
    default:
        counts.tx_refused++;
        return 0; /* busy: the stack sends a SYN, data or FIN again at its next pass */
    }
}

static int mac_init(void *ctx) {
    (void)ctx;
    return 0;
}

/* Frames reach the stack through eth_input(), from net_service(). */
static int mac_poll(void *ctx) {
    (void)ctx;
    return 0;
}

static int mac_peek(void *ctx, uint16_t offset, uint8_t *buf, uint16_t len) {
    (void)ctx;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

static void mac_discard(void *ctx) {
    (void)ctx;
}

static const net_mac_t usb_mac = {mac_init, mac_send, mac_poll, mac_peek, mac_discard, mac_discard};

/* ── Receive [WEB-NET-07] ─────────────────────────────────────────── */

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (!net_rx_take(&rx_slot, src, size, FRAME_MAX)) {
        counts.rx_drops++;
        return false;
    }
    counts.rx_frames++;
    return true;
}

void tud_network_init_cb(void) {
    net_tx_flush(&txq);
    net_rx_done(&rx_slot);
}

/* ── mDNS: pyro.local and _pyro._tcp, renamed on a conflict [WEB-NET-03, WEB-NET-04] ── */

static char mdns_host[24] = "pyro.local";
static char mdns_inst[40] = "pyro._pyro._tcp.local";
static uint8_t mdns_suffix;
static const char *const mdns_txt[] = {NULL};
static const mdns_record_t mdns_records[] = {
    {.type = DNS_TYPE_A, .ttl = MDNS_TTL_HOST, .name = mdns_host, .rdata.a = 0},
    {.type = DNS_TYPE_PTR, .ttl = MDNS_TTL_OTHER, .name = "_pyro._tcp.local", .rdata.ptr = mdns_inst},
    {.type = DNS_TYPE_SRV, .ttl = MDNS_TTL_HOST, .name = mdns_inst, .rdata.srv = {0, 0, 80, mdns_host}},
    {.type = DNS_TYPE_TXT, .ttl = MDNS_TTL_OTHER, .name = mdns_inst, .rdata.txt = mdns_txt},
};
#define MDNS_PTR_RECORD (1u << 1)
static mdns_t mdns;
static bool mdns_start_due;

static void mdns_conflict(mdns_t *m, uint8_t index, void *ctx) {
    (void)index;
    (void)ctx;
    mdns_withdraw(m, MDNS_PTR_RECORD);
    mdns_suffix++;
    snprintf(mdns_host, sizeof(mdns_host), "pyro-%u.local", mdns_suffix);
    snprintf(mdns_inst, sizeof(mdns_inst), "pyro-%u._pyro._tcp.local", mdns_suffix);
    mdns_start_due = true;
}

void net_mdns_poll(void) {
    if (mdns_start_due && tud_ready() && host_settled()) {
        mdns_start_due = false;
        mdns_start(&mdns);
    }
}

/* ── DHCP: the one host on the link [WEB-NET-02] ──────────────────── */

static dhcpv4_server_cfg_t dhcp_cfg;
static dhcpv4_server_t dhcp;

static void dhcp_port(net_t *n, uint32_t src_ip, uint16_t src_port, const uint8_t *src_mac, const uint8_t *payload,
                      uint16_t len) {
    (void)src_port;
    dhcpv4_server_input(n, &dhcp, src_ip, src_mac, payload, len);
}

static void mdns_port(net_t *n, uint32_t src_ip, uint16_t src_port, const uint8_t *src_mac, const uint8_t *payload,
                      uint16_t len) {
    (void)n;
    mdns_input(&mdns, src_ip, src_mac, src_port, payload, len);
}

static const udp_port_entry_t udp_ports[] = {{67, dhcp_port}, {MDNS_PORT, mdns_port}};

/* ── USB lifecycle [WEB-API-13] ───────────────────────────────────── */

static void host_settles(void) {
    settle_until_ms = to_ms_since_boot(get_absolute_time()) + HOST_SETTLE_MS;
}

void tud_mount_cb(void) {
    counts.usb[0]++;
    host_settles();
    mdns_start_due = true;
}

void tud_umount_cb(void) {
    counts.usb[1]++;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    counts.usb[2]++;
}

void tud_resume_cb(void) {
    counts.usb[3]++;
    host_settles();
}

/* ── The net task's calls ─────────────────────────────────────────── */

void net_mac_init(void) {
    memcpy(tud_network_mac_address, board_mac(), sizeof(tud_network_mac_address));
}

/* TCP's initial sequence numbers and mDNS's probe delays: they must differ
 * from boot to boot, not be secret. The ring oscillator's random bit
 * (rp2040-datasheet_2025-02-20.pdf, section 2.17.5, page 223) and the timer
 * [DD-102]. */
static void seed_from_ring_oscillator(void) {
    uint8_t entropy[16];
    for (unsigned i = 0; i < sizeof(entropy); i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++)
            b = (uint8_t)(b << 1 | (rosc_hw->randombit & 1u));
        entropy[i] = b;
    }
    uint64_t t = time_us_64();
    for (unsigned i = 0; i < sizeof(t); i++)
        entropy[i] ^= (uint8_t)(t >> (8u * i));
    net_random_seed(&net, entropy, sizeof(entropy));
}

void net_init(void) {
    uint8_t mac[6];
    memcpy(mac, tud_network_mac_address, sizeof(mac));
    mac[5] ^= 0x01;
    stcp_net_init(&net, rx_frame, sizeof(rx_frame), tx_frame, sizeof(tx_frame), mac, &usb_mac, NULL);
    seed_from_ring_oscillator();

    /* Each board is its own /24, from its MAC [WEB-NET-06]: every board is a
     * point-to-point link with its own DHCP server, and two on one subnet
     * would hand the host the same lease twice. */
    const uint32_t subnet = NET_IPV4(192, 168, board_subnet_octet(), 0);
    net.ipv4_addr = subnet | 1u;
    net.subnet_mask = NET_IPV4(255, 255, 255, 0);
    net.gateway_ipv4 = 0;

    dhcp_cfg = (dhcpv4_server_cfg_t){
        .server_ip = net.ipv4_addr,
        .offered_ip = subnet | 2u,
        .subnet_mask = net.subnet_mask,
        .lease_time_s = 24u * 60u * 60u,
    };
    dhcpv4_server_init(&dhcp, &net, &dhcp_cfg, NULL, NULL);
    mdns_init(&mdns, &net, mdns_records, sizeof(mdns_records) / sizeof(mdns_records[0]), mdns_conflict, NULL);
    udp_set_ports(&net, udp_ports, sizeof(udp_ports) / sizeof(udp_ports[0]));
    http_transport_init(&net, http_server_exchanges());
}

uint32_t net_last_http_us;

void net_service(void) {
    static uint32_t ticked_ms;
    net_tx_drain(&txq, &ep_ops);
    if (rx_slot.frame) {
        eth_input(&net, (uint8_t *)(uintptr_t)rx_slot.frame, rx_slot.len);
        net_rx_done(&rx_slot);
        tud_network_recv_renew();
    }
    /* Every pass, with whatever time has gone by, none included: the tick is
     * also when TCP sends again what the endpoint was too busy to take. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - ticked_ms;
    if (elapsed > 0) {
        ticked_ms = now;
        net_tick(&net, elapsed);
        mdns_tick(&mdns, elapsed);
    } else {
        tcp_tick(&net, 0);
    }
    uint32_t t0 = time_us_32();
    http_server_transport();
    net_tx_drain(&txq, &ep_ops);
    net_last_http_us = time_us_32() - t0;
}

const net_link_counts_t *net_link_counts(void) {
    return &counts;
}
