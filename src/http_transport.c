/*
 * HTTP's links over smallest_tcp. See http_transport.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "http_transport.h"
#include <string.h>

/* What a closing link still offers its peer, and throws away. */
#define DRAIN_WINDOW 1460u
/* A link we have closed, whose peer never finishes: taken back. */
#define DRAIN_MS 5000u

typedef enum { LINK_LISTENING, LINK_WAITING, LINK_BOUND, LINK_DRAINING } link_phase_t;

typedef struct {
    tcp_conn_t tcp; /* MUST be first: the stack's event hands it back */
    uint8_t phase;
    bool closing;       /* our FIN is queued behind the tx ring */
    bool moved;         /* a byte went either way since the last pass */
    bool counted_wait;  /* counted once in conn_full */
    uint16_t in_flight; /* of the tx ring, sent and not yet acknowledged */
    uint32_t seq;       /* connect order: waiting links are served in turn */
    uint32_t last_ms;
    http_conn_t *hc;
} link_t;

static link_t links[HTTP_LINKS];
static tcp_conn_t *table[HTTP_LINKS];
static net_t *stack;
static const http_exchanges_t *xs;
static http_transport_counts_t counts;
static uint32_t link_seq;

static link_t *link_of(void *ctx) {
    return (link_t *)ctx;
}

static bool link_carries(const link_t *l) {
    return l->phase == LINK_BOUND && !l->closing;
}

/* Closed on our side: what the peer still sends is taken and dropped. */
static bool link_swallows(const link_t *l) {
    return l->phase == LINK_DRAINING || l->closing;
}

/* ── The stack's view of the rings (tcp_buf.h) ────────────────────── */

static uint16_t rx_deliver(void *ctx, const uint8_t *data, uint16_t len) {
    link_t *l = link_of(ctx);
    l->moved = true;
    if (link_carries(l))
        return net_ring_write(&l->hc->rx, data, len);
    return link_swallows(l) ? len : 0;
}

static uint16_t rx_read(void *ctx, uint8_t *dst, uint16_t maxlen) {
    (void)ctx;
    (void)dst;
    (void)maxlen;
    return 0; /* the parser reads the ring itself */
}

static uint16_t rx_readable(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    return link_carries(l) ? net_ring_readable(&l->hc->rx) : 0;
}

static uint16_t rx_available(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    if (link_carries(l))
        return net_ring_writable(&l->hc->rx);
    return link_swallows(l) ? DRAIN_WINDOW : 0;
}

static const tcp_rxbuf_ops_t rx_ops = {rx_deliver, rx_read, rx_readable, rx_available};

static uint16_t tx_write(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    (void)data;
    (void)len;
    return 0; /* the handlers fill the ring themselves */
}

/* The response has bytes the handler has not put in the tx ring yet. */
static bool more_to_come(const http_conn_t *hc) {
    return hc->phase == HTTP_SEND && (hc->out_left > 0 || (hc->streaming && hc->stream_left > 0));
}

/* One segment in flight is the stack's rule. Copied out, so a ring that
 * wraps still gives a full one; and a part of one waits for the handler to
 * fill the ring, which an acknowledgement has only just emptied. */
static uint16_t tx_copy_segment(void *ctx, uint8_t *dst, uint16_t mss) {
    link_t *l = link_of(ctx);
    if (l->phase != LINK_BOUND || l->in_flight > 0)
        return 0;
    const net_ring_t *r = &l->hc->tx;
    if (r->len < mss && more_to_come(l->hc))
        return 0;
    uint16_t n = r->len < mss ? r->len : mss;
    uint16_t to_end = (uint16_t)(r->cap - r->tail);
    uint16_t first = n < to_end ? n : to_end;
    memcpy(dst, r->buf + r->tail, first);
    memcpy(dst + first, r->buf, (size_t)(n - first));
    l->in_flight = n;
    return n;
}

static void tx_ack(void *ctx, uint32_t bytes) {
    link_t *l = link_of(ctx);
    uint16_t n = bytes < l->in_flight ? (uint16_t)bytes : l->in_flight;
    if (l->phase == LINK_BOUND)
        net_ring_discard(&l->hc->tx, n);
    l->in_flight = (uint16_t)(l->in_flight - n);
    l->moved = true;
}

static uint16_t tx_in_flight(const void *ctx) {
    return ((const link_t *)ctx)->in_flight;
}

/* The stack is shown one segment at a time, the one tx_copy_segment() would
 * give. It sets PSH only on the segment that empties what it is shown, and
 * with one segment in flight every segment is the last until it is
 * acknowledged: macOS holds the acknowledgement of a full segment without
 * PSH for 100 ms, about one time in fifteen (smallest_tcp issue 9). */
static uint16_t tx_queued(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    if (l->phase != LINK_BOUND)
        return 0;
    if (l->in_flight > 0)
        return l->in_flight;
    uint16_t held = net_ring_readable(&l->hc->tx);
    uint16_t mss = l->tcp.snd_mss;
    if (held < mss)
        return more_to_come(l->hc) ? 0 : held;
    return mss;
}

static uint16_t tx_writable(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    return l->phase == LINK_BOUND ? net_ring_writable(&l->hc->tx) : 0;
}

static void tx_mark_retransmit(void *ctx) {
    link_of(ctx)->in_flight = 0;
    counts.retransmits++;
}

static const tcp_txbuf_ops_t tx_ops = {
    .write = tx_write,
    .ack = tx_ack,
    .in_flight = tx_in_flight,
    .queued = tx_queued,
    .writable = tx_writable,
    .mark_retransmit = tx_mark_retransmit,
    .copy_segment = tx_copy_segment,
};

/* ── Links ────────────────────────────────────────────────────────── */

/* Called inside the stack: it may note, and nothing more. */
static void on_event(tcp_conn_t *tcp, uint8_t events) {
    (void)events;
    ((link_t *)(void *)tcp)->moved = true;
}

static void link_listen(link_t *l) {
    l->phase = LINK_LISTENING;
    l->closing = false;
    l->counted_wait = false;
    l->in_flight = 0;
    l->hc = NULL;
    tcp_conn_init(&l->tcp, &tx_ops, l, &rx_ops, l, on_event);
    tcp_listen(&l->tcp, 80);
}

static void link_free(link_t *l) {
    if (l->hc)
        xs->give_back(l->hc);
    link_listen(l);
}

static void link_abort(link_t *l) {
    counts.http_err++;
    tcp_abort(stack, &l->tcp);
    link_free(l);
}

static bool peer_is_there(tcp_state_t st) {
    return st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT;
}

static link_t *oldest_waiting(void) {
    link_t *oldest = NULL;
    for (int i = 0; i < HTTP_LINKS; i++) {
        link_t *l = &links[i];
        if (l->phase == LINK_WAITING && (!oldest || (int32_t)(l->seq - oldest->seq) < 0))
            oldest = l;
    }
    return oldest;
}

/* Free exchanges go to the connected links in the order they connected. */
static void attach_waiting(void) {
    link_t *l;
    while ((l = oldest_waiting()) != NULL) {
        http_conn_t *hc = xs->take();
        if (!hc) {
            if (!l->counted_wait) {
                counts.conn_full++;
                l->counted_wait = true;
            }
            return;
        }
        l->hc = hc;
        l->phase = LINK_BOUND;
        tcp_window_update(stack, &l->tcp);
    }
}

static void finish_closing(link_t *l, tcp_state_t st, uint32_t now) {
    if (net_ring_readable(&l->hc->tx) == 0) {
        counts.served++;
        xs->give_back(l->hc);
        l->hc = NULL;
        l->phase = LINK_DRAINING;
        l->last_ms = now;
    } else if (st == TCP_CLOSED || now - l->last_ms > HTTP_IDLE_MS) {
        link_abort(l);
    }
}

static void transport_bound(link_t *l, tcp_state_t st, uint32_t now) {
    http_conn_t *hc = l->hc;
    if (xs->held(hc))
        return;
    if (xs->must_let_go(hc)) {
        link_abort(l);
        return;
    }
    if (l->closing) {
        finish_closing(l, st, now);
        return;
    }
    if (!peer_is_there(st)) {
        counts.http_err++;
        link_free(l);
        return;
    }
    hc->rx_eof = st == TCP_CLOSE_WAIT;
    if (http_conn_take_consumed(hc) > 0) {
        tcp_window_update(stack, &l->tcp);
        l->last_ms = now;
    }
    if (l->in_flight == 0 && net_ring_readable(&hc->tx) > 0)
        tcp_output(stack, &l->tcp);
    /* The handler fills the ring after this pass: come straight back for
     * what it adds. */
    if (l->in_flight == 0 && more_to_come(hc) && http_conn_wants_service(hc))
        xs->wake();

    if (hc->failed) {
        link_abort(l);
    } else if (http_conn_done(hc) && net_ring_readable(&hc->tx) == l->in_flight) {
        /* Once the last of the tx ring is on its way: the stack sends a FIN
         * behind what tx_queued() shows it, and that is one segment. */
        tcp_close(stack, &l->tcp);
        l->closing = true;
    } else if (now - l->last_ms > HTTP_IDLE_MS) {
        counts.idle_aborts++;
        link_abort(l);
    }
}

static void transport_link(link_t *l, uint32_t now) {
    tcp_state_t st = tcp_status(&l->tcp);
    if (l->moved) {
        l->moved = false;
        l->last_ms = now;
    }
    switch (l->phase) {
    case LINK_LISTENING:
        if (peer_is_there(st)) {
            counts.accepts++;
            counts.last_accept_ms = now;
            l->seq = ++link_seq;
            l->last_ms = now;
            l->phase = LINK_WAITING;
        } else if (st == TCP_CLOSED) {
            link_listen(l);
        }
        break;
    case LINK_WAITING:
        if (!peer_is_there(st)) {
            link_abort(l);
        } else if (now - l->last_ms > HTTP_IDLE_MS) {
            counts.idle_aborts++;
            link_abort(l);
        }
        break;
    case LINK_BOUND:
        transport_bound(l, st, now);
        break;
    default: /* LINK_DRAINING */
        if (st == TCP_CLOSED || st == TCP_TIME_WAIT) {
            link_listen(l);
        } else if (now - l->last_ms > DRAIN_MS) {
            tcp_abort(stack, &l->tcp);
            link_listen(l);
        }
        break;
    }
}

void http_transport_pass(uint32_t now_ms) {
    for (int i = 0; i < HTTP_LINKS; i++)
        transport_link(&links[i], now_ms);
    attach_waiting();
}

void http_transport_states(uint16_t states[11], uint32_t *nrtx_max) {
    memset(states, 0, 11 * sizeof(states[0]));
    *nrtx_max = 0;
    for (int i = 0; i < HTTP_LINKS; i++) {
        unsigned st = (unsigned)tcp_status(&links[i].tcp);
        if (st < 11u)
            states[st]++;
        if (links[i].tcp.retransmits > *nrtx_max)
            *nrtx_max = links[i].tcp.retransmits;
    }
}

const http_transport_counts_t *http_transport_counts(void) {
    return &counts;
}

void http_transport_init(net_t *net, const http_exchanges_t *x) {
    stack = net;
    xs = x;
    for (int i = 0; i < HTTP_LINKS; i++) {
        link_listen(&links[i]);
        table[i] = &links[i].tcp;
    }
    tcp_set_connections(stack, table, HTTP_LINKS);
}
