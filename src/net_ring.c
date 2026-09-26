/*
 * SPDX-License-Identifier: MIT
 */
#include "net_ring.h"
#include <string.h>

void net_ring_init(net_ring_t *r, uint8_t *buf, uint16_t cap) {
    r->buf = buf;
    r->cap = cap;
    net_ring_clear(r);
}

void net_ring_clear(net_ring_t *r) {
    r->tail = 0;
    r->len = 0;
}

uint16_t net_ring_readable(const net_ring_t *r) {
    return r->len;
}

uint16_t net_ring_writable(const net_ring_t *r) {
    return (uint16_t)(r->cap - r->len);
}

uint16_t net_ring_read_span(const net_ring_t *r, const uint8_t **p) {
    *p = r->buf + r->tail;
    uint16_t to_end = (uint16_t)(r->cap - r->tail);
    return r->len < to_end ? r->len : to_end;
}

uint16_t net_ring_write_span(net_ring_t *r, uint8_t **p) {
    uint16_t head = (uint16_t)((r->tail + r->len) % r->cap);
    *p = r->buf + head;
    uint16_t to_end = (uint16_t)(r->cap - head);
    uint16_t room = net_ring_writable(r);
    return room < to_end ? room : to_end;
}

void net_ring_commit(net_ring_t *r, uint16_t n) {
    uint16_t room = net_ring_writable(r);
    r->len = (uint16_t)(r->len + (n < room ? n : room));
}

void net_ring_discard(net_ring_t *r, uint16_t n) {
    if (n > r->len) {
        n = r->len;
    }
    r->tail = (uint16_t)((r->tail + n) % r->cap);
    r->len = (uint16_t)(r->len - n);
}

uint16_t net_ring_write(net_ring_t *r, const void *src, uint16_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint16_t done = 0;
    while (done < n) {
        uint8_t *p;
        uint16_t span = net_ring_write_span(r, &p);
        if (span == 0) {
            break;
        }
        uint16_t k = (uint16_t)(n - done) < span ? (uint16_t)(n - done) : span;
        memcpy(p, s + done, k);
        net_ring_commit(r, k);
        done = (uint16_t)(done + k);
    }
    return done;
}

uint16_t net_ring_read(net_ring_t *r, void *dst, uint16_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint16_t done = 0;
    while (done < n) {
        const uint8_t *p;
        uint16_t span = net_ring_read_span(r, &p);
        if (span == 0) {
            break;
        }
        uint16_t k = (uint16_t)(n - done) < span ? (uint16_t)(n - done) : span;
        memcpy(d + done, p, k);
        net_ring_discard(r, k);
        done = (uint16_t)(done + k);
    }
    return done;
}
