/*
 * USB network glue for the smallest_tcp spike: TinyUSB ECM/RNDIS frames in
 * and out of the stack, its one-client DHCP server and its mDNS responder.
 * The lwIP build's counterpart is net_glue.c; the two export the same names.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/structs/rosc.h"
#include "board_identity.h"
#include "http_server.h"
#include "mac_random.h"
#include "net_stcp.h"
#include "eth.h"
#include "udp.h"
#include "dhcpv4_server.h"
#include "mdns.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MAX CFG_TUD_NET_MTU
_Static_assert(FRAME_MAX == 1514, "a whole Ethernet frame: an IP MTU of 1500");

static net_t net;
static uint8_t rx_frame[FRAME_MAX];
static uint8_t tx_frame[FRAME_MAX];
static uint16_t rx_waiting; /* bytes of rx_frame the stack has not seen yet */

volatile uint32_t net_rx_count;
volatile uint32_t net_rx_drop;
volatile uint32_t net_tx_fail;
volatile uint32_t net_tx_ok;
volatile uint32_t net_tx_held;
volatile uint32_t net_usb_events[4];
volatile uint32_t net_http_served;

uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x00};

net_t *net_stcp(void) {
    return &net;
}

/* ── Transmit ─────────────────────────────────────────────────────────
 *
 * TCP takes a frame the driver refuses for a lost segment, and finds out by
 * its retransmission timer, a second at best. So a frame the endpoint cannot
 * take yet is copied here and sent in order once it can. */

#define TXQ_BYTES 8192u
static uint8_t txq[TXQ_BYTES];
static uint16_t txq_head, txq_tail;
static uint32_t settle_until_ms; /* 0: the host has had its time since the mount */

#define HOST_SETTLE_MS 500u

static bool host_settled(void) {
    if (!tud_ready())
        return false;
    if (settle_until_ms != 0) {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - settle_until_ms) < 0)
            return false;
        settle_until_ms = 0;
    }
    return true;
}

static bool endpoint_takes(uint16_t len) {
    return host_settled() && tud_network_can_xmit(len);
}

static void txq_drain(void) {
    while (txq_head < txq_tail) {
        uint16_t len = (uint16_t)(txq[txq_head] | txq[txq_head + 1] << 8);
        if (!endpoint_takes(len))
            return;
        tud_network_xmit(&txq[txq_head + 2], len);
        net_tx_ok++;
        txq_head = (uint16_t)(txq_head + 2u + len);
    }
    txq_head = txq_tail = 0;
}

static bool txq_hold(const uint8_t *frame, uint16_t len) {
    uint16_t need = (uint16_t)(len + 2u);
    if (TXQ_BYTES - txq_tail < need && txq_head > 0) {
        memmove(txq, &txq[txq_head], (size_t)(txq_tail - txq_head));
        txq_tail = (uint16_t)(txq_tail - txq_head);
        txq_head = 0;
    }
    if (TXQ_BYTES - txq_tail < need)
        return false;
    txq[txq_tail] = (uint8_t)len;
    txq[txq_tail + 1] = (uint8_t)(len >> 8);
    memcpy(&txq[txq_tail + 2], frame, len);
    txq_tail = (uint16_t)(txq_tail + need);
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    memcpy(dst, ref, arg);
    return arg;
}

static int mac_init(void *ctx) {
    (void)ctx;
    return 0;
}

static int mac_send(void *ctx, const uint8_t *frame, uint16_t len) {
    (void)ctx;
    if (len > FRAME_MAX) {
        net_tx_fail++;
        return -1;
    }
    if (txq_head == txq_tail && endpoint_takes(len)) {
        tud_network_xmit((void *)(uintptr_t)frame, len);
        net_tx_ok++;
        return len;
    }
    if (!txq_hold(frame, len)) {
        net_tx_fail++;
        return 0;
    }
    net_tx_held++;
    return len;
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

/* ── Receive ──────────────────────────────────────────────────────────
 *
 * TinyUSB holds its endpoint until tud_network_recv_renew(), so one frame
 * waits at a time. It is copied into the stack's own receive buffer: mdns.c
 * reads the IP header of the frame it is handed from net->rx.buf. */

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (rx_waiting != 0 || size == 0 || size > FRAME_MAX) {
        net_rx_drop++;
        return false;
    }
    memcpy(rx_frame, src, size);
    rx_waiting = size;
    net_rx_count++;
    return true;
}

void tud_network_init_cb(void) {
    txq_head = txq_tail = 0;
    rx_waiting = 0;
}

/* ── mDNS: pyro.local and _pyro._tcp, renamed on a conflict ───────── */

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

/* ── DHCP: the one host on the link ───────────────────────────────── */

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

/* ── USB lifecycle ────────────────────────────────────────────────── */

void tud_mount_cb(void) {
    net_usb_events[0]++;
    settle_until_ms = to_ms_since_boot(get_absolute_time()) + HOST_SETTLE_MS;
    mdns_start_due = true;
}

void tud_umount_cb(void) {
    net_usb_events[1]++;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    net_usb_events[2]++;
}

void tud_resume_cb(void) {
    net_usb_events[3]++;
    settle_until_ms = to_ms_since_boot(get_absolute_time()) + HOST_SETTLE_MS;
}

/* ── What hal_common.c calls ──────────────────────────────────────── */

void net_mac_init(void) {
    memcpy(tud_network_mac_address, board_mac(), sizeof(tud_network_mac_address));
}

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
    mac[5] ^= 0x01; /* the two ends of the link differ */
    stcp_net_init(&net, rx_frame, sizeof(rx_frame), tx_frame, sizeof(tx_frame), mac, &usb_mac, NULL);
    seed_from_ring_oscillator();

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
}

void net_start(void) {}

/* Started once the host can hear the probes. */
void net_mdns_poll(void) {
    if (mdns_start_due && host_settled()) {
        mdns_start_due = false;
        mdns_start(&mdns);
    }
}

uint32_t net_last_http_us;

void net_service(void) {
    static uint32_t ticked_ms;
    txq_drain();
    if (rx_waiting != 0) {
        uint16_t len = rx_waiting;
        eth_input(&net, rx_frame, len);
        rx_waiting = 0;
        tud_network_recv_renew();
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - ticked_ms;
    if (elapsed > 0) {
        ticked_ms = now;
        net_tick(&net, elapsed);
        mdns_tick(&mdns, elapsed);
    }
    uint32_t t0 = time_us_32();
    http_server_transport();
    txq_drain();
    net_last_http_us = time_us_32() - t0;
}

void lwip_uart_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        hal_telemetry_send(buf);
}
