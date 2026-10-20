/*
 * The USB network's frames (net_txq.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_txq.h"
#include <string.h>

#define LEN_BYTES 2u

static uint16_t len_at(const net_txq_t *q, uint16_t at) {
    return (uint16_t)(q->buf[at] | q->buf[at + 1u] << 8);
}

void net_tx_flush(net_txq_t *q) {
    q->head = q->tail = 0;
    q->n = 0;
}

uint8_t net_tx_held(const net_txq_t *q) {
    return q->n;
}

void net_tx_drain(net_txq_t *q, const net_tx_ops_t *ops) {
    if (!ops->ready()) {
        net_tx_flush(q);
        return;
    }
    while (q->n && ops->can_send(len_at(q, q->head))) {
        uint16_t len = len_at(q, q->head);
        ops->send(&q->buf[q->head + LEN_BYTES], len);
        q->head = (uint16_t)(q->head + LEN_BYTES + len);
        q->n--;
    }
    if (q->n == 0)
        q->head = q->tail = 0;
}

/* Room for need bytes at the tail, the held frames moved to the front if
 * that makes it. */
static bool make_room(net_txq_t *q, uint16_t need) {
    if (NET_TXQ_BYTES - q->tail >= need)
        return true;
    memmove(q->buf, &q->buf[q->head], (size_t)(q->tail - q->head));
    q->tail = (uint16_t)(q->tail - q->head);
    q->head = 0;
    return NET_TXQ_BYTES - q->tail >= need;
}

net_tx_result_t net_tx_offer(net_txq_t *q, const net_tx_ops_t *ops, const uint8_t *frame, uint16_t len) {
    net_tx_drain(q, ops);
    if (!ops->ready())
        return NET_TX_NOT_READY;
    if (q->n == 0 && ops->can_send(len)) {
        ops->send(frame, len);
        return NET_TX_SENT;
    }
    uint32_t need = LEN_BYTES + (uint32_t)len;
    if (need > NET_TXQ_BYTES || !make_room(q, (uint16_t)need))
        return NET_TX_FULL;
    q->buf[q->tail] = (uint8_t)len;
    q->buf[q->tail + 1u] = (uint8_t)(len >> 8);
    memcpy(&q->buf[q->tail + LEN_BYTES], frame, len);
    q->tail = (uint16_t)(q->tail + need);
    q->n++;
    return NET_TX_HELD;
}

bool net_rx_take(net_rx_slot_t *s, const uint8_t *src, uint16_t size, uint16_t max) {
    if (s->frame || size == 0 || size > max)
        return false;
    s->frame = src;
    s->len = size;
    return true;
}

void net_rx_done(net_rx_slot_t *s) {
    s->frame = NULL;
    s->len = 0;
}

uint16_t net_tx_copy_len(uint32_t frame_len, uint16_t cap) {
    return frame_len <= cap ? (uint16_t)frame_len : 0u;
}
