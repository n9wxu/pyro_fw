/*
 * /api/net [WEB-API-13]: the network's counters, for the bench. They say
 * which of lwIP's pools, which TCP states, and which transport step refused,
 * so an HTTP outage can be told apart.
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
#define NET_JSON_MAX 1024

/* One of lwIP's pools: in use, its high-water mark, allocations refused. */
typedef struct {
    uint32_t used, max, err;
} net_pool_t;

typedef struct {
    net_pool_t heap, tcp_pcb, tcp_seg, pbuf_pool;
    uint32_t tcp_xmit, tcp_recv, tcp_drop, tcp_memerr;
    uint32_t icmp_recv, icmp_xmit;
    uint16_t states[11]; /* connections in each TCP state, CLOSED through TIME_WAIT */
    uint32_t sndq_max;   /* the longest send queue, in segments */
    uint32_t nrtx_max;   /* the most retransmissions any connection is on */
    uint32_t accepts, accept_refused, write_fails, conn_full, http_err, idle_aborts;
    uint32_t rx_frames, rx_drops;
    uint32_t tx_sent, tx_held, tx_refused; /* held: waited for the endpoint (net_txq.h) */
    uint32_t usb[4]; /* mounts, unmounts, suspends, resumes */
    uint32_t last_accept_ms;
} net_snap_t;

/* The JSON's length, or 0 if it does not fit in n. */
int net_json(const net_snap_t *s, char *out, size_t n);

#endif /* NET_STATS_H */
