/*
 * The USB network's transmit queue [WEB-NET-05, DD-070].
 *
 * A frame the endpoint cannot take yet is held, by reference, and sent in
 * order as soon as it can, rather than dropped and left to TCP's
 * retransmission timer.
 *
 * The endpoint and the frames' references are the caller's, so the host
 * tests this against a fake one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_TXQ_H
#define NET_TXQ_H

#include <stdbool.h>
#include <stdint.h>

#define NET_TXQ_N 8

typedef struct {
    void *frame[NET_TXQ_N];
    uint8_t head, n;
} net_txq_t;

typedef struct {
    bool (*ready)(void);           /* the host has configured the device */
    bool (*can_send)(void *frame); /* the endpoint takes this frame now */
    void (*send)(void *frame);     /* copies it out; the caller keeps it */
    void (*hold)(void *frame);     /* a reference while it waits */
    void (*release)(void *frame);
} net_tx_ops_t;

typedef enum {
    NET_TX_SENT,      /* gone */
    NET_TX_HELD,      /* waiting for the endpoint */
    NET_TX_FULL,      /* refused: TCP's retransmission resends it */
    NET_TX_NOT_READY, /* refused: the link is gone, and the queue with it */
} net_tx_result_t;

net_tx_result_t net_tx_offer(net_txq_t *q, const net_tx_ops_t *ops, void *frame);

/* Sends what the endpoint takes, oldest first. Once a loop and after the
 * transport. */
void net_tx_drain(net_txq_t *q, const net_tx_ops_t *ops);

/* Releases everything held: the link it was for is gone. */
void net_tx_flush(net_txq_t *q, const net_tx_ops_t *ops);

typedef struct {
    void *(*alloc)(uint16_t size);
    bool (*fill)(void *frame, const uint8_t *src, uint16_t size);
    void (*release)(void *frame);
} net_rx_ops_t;

bool net_rx_take(void **slot, const net_rx_ops_t *ops, const uint8_t *src, uint16_t size);

uint16_t net_tx_copy_len(uint32_t frame_len, uint16_t cap);

#endif /* NET_TXQ_H */
