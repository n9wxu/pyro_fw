/*
 * HTTP's links over smallest_tcp [WEB-HTTP-01..03, DD-102].
 *
 * A link is one of the stack's connections, listening on port 80 until a
 * peer takes it. A connected link is given an exchange by the server; until
 * then its window stays shut, which holds the sender. The stack's buffer
 * operations are written over the exchange's own rings, so a byte is copied
 * once each way, and a sent byte stays in the tx ring until it is
 * acknowledged.
 *
 * The stack has no accept queue: a SYN that finds every link in use is
 * reset. There are HTTP_LINKS of them against the server's few exchanges.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_TRANSPORT_H
#define HTTP_TRANSPORT_H

#include "http_conn.h"
#include "net_stcp.h"
#include <stdbool.h>
#include <stdint.h>

#define HTTP_LINKS 16
/* No byte moved either way for this long: the peer is gone. */
#define HTTP_IDLE_MS 20000u

/* The server's exchanges, as the transport sees them. */
typedef struct http_exchanges {
    http_conn_t *(*take)(void);                 /* a free exchange, initialised; NULL while all are busy */
    void (*give_back)(http_conn_t *hc);         /* the link is done with it */
    bool (*held)(const http_conn_t *hc);        /* a work unit has it: the transport leaves it alone */
    bool (*must_let_go)(const http_conn_t *hc); /* it holds the filesystem the flight log needs [WEB-API-08] */
    void (*wake)(void);                         /* a pass soon: the handler is filling the tx ring */
} http_exchanges_t;

typedef struct {
    uint32_t accepts;     /* peers connected */
    uint32_t conn_full;   /* links that waited for an exchange */
    uint32_t http_err;    /* links aborted */
    uint32_t idle_aborts; /* of which, idle for HTTP_IDLE_MS */
    uint32_t served;      /* responses acknowledged whole */
    uint32_t retransmits; /* segments the stack sent again */
    uint32_t last_accept_ms;
} http_transport_counts_t;

void http_transport_init(net_t *net, const http_exchanges_t *x);

/* Once a net task pass, after the stack has taken the frames received. */
void http_transport_pass(uint32_t now_ms);

/* Each link's TCP state counted into states (CLOSED through TIME_WAIT), and
 * the most retransmissions any link is on. */
void http_transport_states(uint16_t states[11], uint32_t *nrtx_max);

const http_transport_counts_t *http_transport_counts(void);

#endif
