/*
 * The USB network's transmit queue [G4-N].
 *
 * A frame the endpoint cannot take yet is held, by reference, and sent in
 * order as soon as it can. Dropped instead, lwIP learns of it only by its
 * retransmit timer, 3 s and doubling, and the heap stays held meanwhile:
 * under G4's load 40 % of frames were refused and the heap ran out 27,865
 * times. lwIP does not retransmit a segment a driver still holds.
 *
 * Portable: the endpoint and the frames' references are the caller's, so the
 * host tests this against a fake one.
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
    NET_TX_FULL,      /* refused: lwIP's retransmit has it */
    NET_TX_NOT_READY, /* refused: the link is gone, and the queue with it */
} net_tx_result_t;

net_tx_result_t net_tx_offer(net_txq_t *q, const net_tx_ops_t *ops, void *frame);

/* Sends what the endpoint takes, oldest first. Once a loop and after the
 * transport. */
void net_tx_drain(net_txq_t *q, const net_tx_ops_t *ops);

/* Releases everything held: the link it was for is gone. */
void net_tx_flush(net_txq_t *q, const net_tx_ops_t *ops);

#endif /* NET_TXQ_H */
