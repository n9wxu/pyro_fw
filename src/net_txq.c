/*
 * The USB network's transmit queue (net_txq.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_txq.h"

void net_tx_flush(net_txq_t *q, const net_tx_ops_t *ops) {
    while (q->n) {
        ops->release(q->frame[q->head]);
        q->head = (uint8_t)((q->head + 1u) % NET_TXQ_N);
        q->n--;
    }
    q->head = 0;
}

void net_tx_drain(net_txq_t *q, const net_tx_ops_t *ops) {
    if (!ops->ready()) {
        net_tx_flush(q, ops);
        return;
    }
    while (q->n && ops->can_send(q->frame[q->head])) {
        void *f = q->frame[q->head];
        ops->send(f);
        ops->release(f);
        q->head = (uint8_t)((q->head + 1u) % NET_TXQ_N);
        q->n--;
    }
}

net_tx_result_t net_tx_offer(net_txq_t *q, const net_tx_ops_t *ops, void *frame) {
    net_tx_drain(q, ops);
    if (!ops->ready())
        return NET_TX_NOT_READY;
    if (q->n == 0 && ops->can_send(frame)) {
        ops->send(frame);
        return NET_TX_SENT;
    }
    if (q->n == NET_TXQ_N)
        return NET_TX_FULL;
    ops->hold(frame);
    q->frame[(q->head + q->n) % NET_TXQ_N] = frame;
    q->n++;
    return NET_TX_HELD;
}

bool net_rx_take(void **slot, const net_rx_ops_t *ops, const uint8_t *src, uint16_t size) {
    if (*slot || size == 0)
        return false;
    void *f = ops->alloc(size);
    if (!f)
        return false;
    if (!ops->fill(f, src, size)) {
        ops->release(f);
        return false;
    }
    *slot = f;
    return true;
}

uint16_t net_tx_copy_len(uint32_t frame_len, uint16_t cap) {
    return frame_len <= cap ? (uint16_t)frame_len : 0u;
}
