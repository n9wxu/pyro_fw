/*
 * /api/net's rendering (net_stats.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_stats.h"
#include <stdio.h>

#define U(x) ((unsigned long)(x))
#define POOL(p) U((p).used), U((p).max), U((p).err)

int net_json(const net_snap_t *s, char *out, size_t n) {
    const uint16_t *st = s->states;
    int len = snprintf(
        out, n,
        "{\"heap\":[%lu,%lu,%lu],\"tcp_pcb\":[%lu,%lu,%lu],\"tcp_seg\":[%lu,%lu,%lu],\"pbuf_pool\":[%lu,%lu,%lu],"
        "\"tcp\":{\"xmit\":%lu,\"recv\":%lu,\"drop\":%lu,\"memerr\":%lu},\"icmp\":[%lu,%lu],"
        "\"states\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"sndq_max\":%lu,\"nrtx_max\":%lu,"
        "\"accepts\":%lu,\"accept_refused\":%lu,\"write_fails\":%lu,\"conn_full\":%lu,\"http_err\":%lu,"
        "\"idle_aborts\":%lu,\"rx\":[%lu,%lu],\"tx\":[%lu,%lu,%lu],\"usb\":[%lu,%lu,%lu,%lu],\"last_accept_ms\":%lu}",
        POOL(s->heap), POOL(s->tcp_pcb), POOL(s->tcp_seg), POOL(s->pbuf_pool), U(s->tcp_xmit), U(s->tcp_recv),
        U(s->tcp_drop), U(s->tcp_memerr), U(s->icmp_recv), U(s->icmp_xmit), st[0], st[1], st[2], st[3], st[4], st[5],
        st[6], st[7], st[8], st[9], st[10], U(s->sndq_max), U(s->nrtx_max), U(s->accepts), U(s->accept_refused),
        U(s->write_fails), U(s->conn_full), U(s->http_err), U(s->idle_aborts), U(s->rx_frames), U(s->rx_drops),
        U(s->tx_sent), U(s->tx_held), U(s->tx_refused), U(s->usb[0]), U(s->usb[1]), U(s->usb[2]), U(s->usb[3]),
        U(s->last_accept_ms));
    if (len < 0 || (size_t)len >= n) {
        if (n)
            out[0] = '\0';
        return 0;
    }
    return len;
}
