/*
 * The USB network: TinyUSB's ECM/RNDIS frames in and out of smallest_tcp,
 * its one-client DHCP server and its mDNS responder [WEB-NET-01..07,
 * DD-102]. Run by the net task.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_GLUE_H
#define NET_GLUE_H

#include <stdint.h>

typedef struct {
    uint32_t rx_frames, rx_drops;
    uint32_t tx_sent, tx_held, tx_refused; /* held: waited for the endpoint (net_txq.h) */
    uint32_t usb[4];                       /* mounts, unmounts, suspends, resumes */
} net_link_counts_t;

/* Before tud_init(): the MAC goes into the ECM descriptor, which the host
 * reads once, at enumeration. */
void net_mac_init(void);

/* The stack, its addresses, DHCP, mDNS and the HTTP transport. */
void net_init(void);

/* One pass: frames each way, the stack's timers, the HTTP transport. */
void net_service(void);

/* mDNS's probes, once the host can hear them. */
void net_mdns_poll(void);

const net_link_counts_t *net_link_counts(void);

/* How long the last pass's HTTP transport took, for the net task's
 * breakdown. */
extern uint32_t net_last_http_us;

#endif
