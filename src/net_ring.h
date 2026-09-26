/*
 * A byte ring: one producer, one consumer, both on core0.
 *
 * The HTTP layer owns a pair of these per connection. The transport fills the
 * RX ring and drains the TX ring; the HTTP parser drains RX and the response
 * generator fills TX. Neither side sees how the other chunks its data, which
 * is the point: TCP is a stream.
 *
 * The operations follow smallest_tcp's buffer vtables (tcp_buf.h), so these
 * rings can become that stack's TCP buffers:
 *
 *   tcp_rxbuf_ops_t  deliver = net_ring_write    read      = net_ring_read
 *                    readable = net_ring_readable available = net_ring_writable
 *   tcp_txbuf_ops_t  write   = net_ring_write    writable  = net_ring_writable
 *
 * The TX ring there also keeps sent bytes until they are ACKed; lwIP keeps
 * its own copy instead, so here a byte leaves the ring once lwIP has it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_RING_H
#define NET_RING_H

#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t tail; /* next byte to read */
    uint16_t len;  /* bytes held        */
} net_ring_t;

void net_ring_init(net_ring_t *r, uint8_t *buf, uint16_t cap);
void net_ring_clear(net_ring_t *r);

uint16_t net_ring_readable(const net_ring_t *r);
uint16_t net_ring_writable(const net_ring_t *r);

/* Both return what was moved, which may be less than n. */
uint16_t net_ring_write(net_ring_t *r, const void *src, uint16_t n);
uint16_t net_ring_read(net_ring_t *r, void *dst, uint16_t n);
void net_ring_discard(net_ring_t *r, uint16_t n);

/* The longest run that can be read, or written, in place. A producer writes
 * into the span it was given and then commits what it wrote. */
uint16_t net_ring_read_span(const net_ring_t *r, const uint8_t **p);
uint16_t net_ring_write_span(net_ring_t *r, uint8_t **p);
void net_ring_commit(net_ring_t *r, uint16_t n);

#endif
