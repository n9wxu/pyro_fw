/*
 * The HTTP server's transport over smallest_tcp, in place of the lwIP adapter
 * in http_server.c, which includes this file when PYRO_NET_SMALLEST_TCP is
 * set. A spike: see docs/smallest_tcp_evaluation.md.
 *
 * The stack's buffer operations are written over the exchange's own rings
 * (net_ring.h), so a byte is copied once each way: from the received frame
 * into the rx ring, and from the tx ring into the frame sent, across the
 * ring's wrap. A sent byte stays in the tx ring until it is acknowledged.
 *
 * A link has no buffer of its own. Until it is given an exchange its window
 * is zero, which holds its sender; given one, the window opens to the rx
 * ring's free space.
 *
 * SPDX-License-Identifier: MIT
 */

/* What a closing link still offers its peer, and throws away. */
#define DRAIN_WINDOW 1460u
/* A link we have closed, whose peer never finishes: taken back. */
#define DRAIN_MS 5000u

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr); /* wakes the net task */

static net_t *stcp;
static tcp_conn_t *stcp_table[LINK_POOL_SIZE];

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
        return net_ring_write(&l->conn->h.rx, data, len);
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
    return link_carries(l) ? net_ring_readable(&l->conn->h.rx) : 0;
}

static uint16_t rx_available(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    if (link_carries(l))
        return net_ring_writable(&l->conn->h.rx);
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
    const net_ring_t *r = &l->conn->h.tx;
    if (r->len < mss && more_to_come(&l->conn->h))
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
        net_ring_discard(&l->conn->h.tx, n);
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
 * PSH for 100 ms, about one time in fifteen. */
static uint16_t tx_queued(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    if (l->phase != LINK_BOUND)
        return 0;
    if (l->in_flight > 0)
        return l->in_flight;
    uint16_t held = net_ring_readable(&l->conn->h.tx);
    uint16_t mss = l->tcp.snd_mss;
    if (held < mss)
        return more_to_come(&l->conn->h) ? 0 : held;
    return mss;
}

static uint16_t tx_writable(const void *ctx) {
    const link_t *l = (const link_t *)ctx;
    return l->phase == LINK_BOUND ? net_ring_writable(&l->conn->h.tx) : 0;
}

static void tx_mark_retransmit(void *ctx) {
    link_of(ctx)->in_flight = 0;
    stcp_retransmits++;
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

/* ── /api/net ─────────────────────────────────────────────────────── */

/* The stack has no pools to report; the keys stay, at zero, for the scripts
 * that read them. */
static void serve_api_net(http_conn_t *hc) {
    net_snap_t s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < LINK_POOL_SIZE; i++) {
        unsigned st = (unsigned)tcp_status(&links[i].tcp);
        if (st < sizeof(s.states) / sizeof(s.states[0]))
            s.states[st]++;
        if (links[i].tcp.retransmits > s.nrtx_max)
            s.nrtx_max = links[i].tcp.retransmits;
    }
    s.tcp_drop = stcp_retransmits;
    s.accepts = net_http_accept;
    s.accept_refused = net_accept_refused;
    s.write_fails = net_write_fails;
    s.conn_full = net_conn_full;
    s.http_err = net_http_err;
    s.idle_aborts = net_idle_aborts;
    s.rx_frames = net_rx_count;
    s.rx_drops = net_rx_drop;
    s.tx_sent = net_tx_ok;
    s.tx_held = net_tx_held;
    s.tx_refused = net_tx_fail;
    for (int i = 0; i < 4; i++)
        s.usb[i] = net_usb_events[i];
    s.last_accept_ms = net_last_accept_ms;
    int n = net_json(&s, (char *)hc->work, sizeof(hc->work));
    http_respond(hc, 200, JSON, hc->work, (uint32_t)n);
}

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
    l->conn = NULL;
    tcp_conn_init(&l->tcp, &tx_ops, l, &rx_ops, l, on_event);
    tcp_listen(&l->tcp, 80);
}

/* An exchange the worker holds is only unlinked; it is released once the
 * worker gives it back (take_back_from_worker()). */
static void link_free(link_t *l) {
    conn_t *c = l->conn;
    if (c && http_work_held(slot_of(c))) {
        c->orphan = true;
        c->link = NULL;
    } else if (c) {
        conn_release(c);
    }
    link_listen(l);
}

static void link_abort(link_t *l) {
    net_http_err++;
    tcp_abort(stcp, &l->tcp);
    link_free(l);
}

static bool peer_is_there(tcp_state_t st) {
    return st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT;
}

/* Free exchanges go to the connected links in the order they connected. */
static void attach_waiting(void) {
    for (;;) {
        conn_t *c = NULL;
        for (int i = 0; i < CONN_POOL_SIZE && !c; i++) {
            if (!conns[i].link && !conns[i].orphan && !http_work_held(i))
                c = &conns[i];
        }
        link_t *oldest = NULL;
        for (int i = 0; i < LINK_POOL_SIZE; i++) {
            link_t *l = &links[i];
            if (l->phase == LINK_WAITING && (!oldest || (int32_t)(l->seq - oldest->seq) < 0))
                oldest = l;
        }
        if (!oldest)
            return;
        if (!c) {
            if (!oldest->counted_wait) {
                net_conn_full++;
                oldest->counted_wait = true;
            }
            return;
        }
        conn_bind(c, oldest);
        oldest->phase = LINK_BOUND;
        tcp_window_update(stcp, &oldest->tcp);
    }
}

static void transport_bound(link_t *l, tcp_state_t st, uint32_t now) {
    conn_t *c = l->conn;
    if (http_work_held(slot_of(c)))
        return;
    http_conn_t *hc = &c->h;

    /* [WEB-API-08, DD-058] As the lwIP adapter: a request holding the
     * filesystem lets go when the log takes it. */
    if (c->fs_held && hal_fs_locked()) {
        link_abort(l);
        return;
    }
    if (l->closing) {
        bool all_acknowledged = net_ring_readable(&hc->tx) == 0;
        if (all_acknowledged) {
            net_http_served++;
            conn_release(c);
            l->phase = LINK_DRAINING;
            l->last_ms = now;
        } else if (st == TCP_CLOSED || now - l->last_ms > HTTP_IDLE_MS) {
            link_abort(l);
        }
        return;
    }
    if (!peer_is_there(st)) {
        net_http_err++;
        link_free(l);
        return;
    }
    hc->rx_eof = st == TCP_CLOSE_WAIT;
    if (http_conn_take_consumed(hc) > 0) {
        tcp_window_update(stcp, &l->tcp);
        l->last_ms = now;
    }
    if (l->in_flight == 0 && net_ring_readable(&hc->tx) > 0)
        tcp_output(stcp, &l->tcp);
    /* The handler fills the ring after this pass: come straight back for
     * what it adds, without the task's wait. */
    if (l->in_flight == 0 && more_to_come(hc) && http_conn_wants_service(hc))
        tud_event_hook_cb(0, 0, false);

    if (hc->failed) {
        link_abort(l);
    } else if (http_conn_done(hc) && net_ring_readable(&hc->tx) == l->in_flight) {
        /* Once the last of the tx ring is on its way: the stack sends a FIN
         * behind what tx_queued() shows it, and that is one segment. */
        tcp_close(stcp, &l->tcp);
        l->closing = true;
    } else if (now - l->last_ms > HTTP_IDLE_MS) {
        net_idle_aborts++;
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
            net_http_accept++;
            net_last_accept_ms = now;
            l->seq = ++link_seq;
            l->last_ms = now;
            l->phase = LINK_WAITING;
        } else if (st == TCP_CLOSED) {
            link_listen(l);
        }
        break;
    case LINK_WAITING:
        if (!peer_is_there(st) || now - l->last_ms > HTTP_IDLE_MS)
            link_abort(l);
        break;
    case LINK_BOUND:
        transport_bound(l, st, now);
        break;
    default: /* LINK_DRAINING */
        if (st == TCP_CLOSED || st == TCP_TIME_WAIT) {
            link_listen(l);
        } else if (now - l->last_ms > DRAIN_MS) {
            tcp_abort(stcp, &l->tcp);
            link_listen(l);
        }
        break;
    }
}

void http_server_transport(void) {
    uint32_t now = hal_time_ms();
    take_back_from_worker();
    for (int i = 0; i < LINK_POOL_SIZE; i++)
        transport_link(&links[i], now);
    attach_waiting();
}

/* No accept queue in the stack: a SYN that finds every link in use is reset,
 * so there are as many links as lwIP had pcbs. */
void http_server_init(void) {
    stcp = net_stcp();
    for (int i = 0; i < LINK_POOL_SIZE; i++) {
        link_listen(&links[i]);
        stcp_table[i] = &links[i].tcp;
    }
    tcp_set_connections(stcp, stcp_table, LINK_POOL_SIZE);
}
