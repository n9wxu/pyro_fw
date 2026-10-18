/*
 * The USB network's frames [WEB-NET-05, WEB-NET-07, DD-102].
 *
 * Sent: a frame the endpoint cannot take yet is copied and held, in
 * NET_TXQ_BYTES, and sent in order as soon as it can. smallest_tcp sends a
 * refused SYN, data or FIN again at its next pass, but not a bare
 * acknowledgement or a window update, and a lost window update holds a host
 * at a shut window until its own probe, seconds later. Those frames are
 * small; a full segment never fits, and TCP has it.
 *
 * Received: TinyUSB holds its endpoint, and the frame in its buffer, until
 * tud_network_recv_renew(), so one frame waits at a time, read where it
 * lies.
 *
 * Portable: the endpoint is the caller's, so the host tests this against a
 * fake one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_TXQ_H
#define NET_TXQ_H

#include <stdbool.h>
#include <stdint.h>

#define NET_TXQ_BYTES 256

typedef struct {
    uint8_t buf[NET_TXQ_BYTES]; /* each frame its length, two bytes, then itself */
    uint16_t head, tail;
    uint8_t n;
} net_txq_t;

typedef struct {
    bool (*ready)(void);                              /* the host has configured the device */
    bool (*can_send)(uint16_t len);                   /* the endpoint takes a frame this long now */
    void (*send)(const uint8_t *frame, uint16_t len); /* copies it out */
} net_tx_ops_t;

typedef enum {
    NET_TX_SENT,      /* gone */
    NET_TX_HELD,      /* copied, waiting for the endpoint */
    NET_TX_FULL,      /* refused: it does not fit */
    NET_TX_NOT_READY, /* refused: the link is gone, and the queue with it */
} net_tx_result_t;

net_tx_result_t net_tx_offer(net_txq_t *q, const net_tx_ops_t *ops, const uint8_t *frame, uint16_t len);

/* Sends what the endpoint takes, oldest first. */
void net_tx_drain(net_txq_t *q, const net_tx_ops_t *ops);

/* Drops everything held: the link it was for is gone. */
void net_tx_flush(net_txq_t *q);

uint8_t net_tx_held(const net_txq_t *q);

/* ── Received frames ──────────────────────────────────────────────── */

typedef struct {
    const uint8_t *frame; /* NULL: none waits */
    uint16_t len;
} net_rx_slot_t;

/* The endpoint's frame, held in its own buffer. True only when the slot now
 * holds it: TinyUSB then waits for tud_network_recv_renew(), and after a
 * false it renews the endpoint itself (TinyUSB's ecm_rndis_device.c). */
bool net_rx_take(net_rx_slot_t *s, const uint8_t *src, uint16_t size, uint16_t max);

/* The stack has read it. */
void net_rx_done(net_rx_slot_t *s);

/* What may be copied into a cap-byte endpoint buffer: the whole frame, or 0
 * when it does not fit. */
uint16_t net_tx_copy_len(uint32_t frame_len, uint16_t cap);

#endif /* NET_TXQ_H */
