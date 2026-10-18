/*
 * /api/net: the network's counters, for the bench [WEB-API-13]. An HTTP
 * outage leaves nothing on the board to say what went wrong; these say which
 * TCP states the links are in, what was sent again, what the transport
 * refused, and what the USB link carried.
 *
 * A snapshot, rendered by net_json(), which touches nothing else, so the
 * host tests the API it produces.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_STATS_H
#define NET_STATS_H

#include <stddef.h>
#include <stdint.h>

/* The widest rendering, with room to spare. */
#define NET_JSON_MAX 640

typedef struct {
    uint16_t states[11];  /* links in each TCP state, CLOSED through TIME_WAIT */
    uint32_t retransmits; /* segments sent again */
    uint32_t nrtx_max;    /* the most retransmissions any link is on */
    uint32_t accepts, conn_full, http_err, idle_aborts, served;
    uint32_t rx_frames, rx_drops;
    uint32_t tx_sent, tx_held, tx_refused; /* held: waited for the endpoint (net_txq.h) */
    uint32_t usb[4];                       /* mounts, unmounts, suspends, resumes */
    uint32_t last_accept_ms;
} net_snap_t;

/* The JSON's length, or 0 if it does not fit in n. */
int net_json(const net_snap_t *s, char *out, size_t n);

#endif /* NET_STATS_H */
