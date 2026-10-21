/*
 * The USB network's frames (net_txq.c), against a fake endpoint. Sent: a
 * frame the endpoint cannot take yet is held, in 256 bytes, and sent in order
 * as soon as it can [WEB-NET-05, DD-102]. Received: the receive callback says
 * it holds a frame only when it does [WEB-NET-07].
 */
#include "unity.h"
#include "net_txq.h"
#include <string.h>

static struct {
    bool ready;        /* the device is configured */
    int room;          /* frames the endpoint takes before it is busy */
    uint8_t sent[128]; /* the first byte of each frame, in the order they went out */
    uint16_t len[128];
    int n_sent;
} ep;

static bool f_ready(void) {
    return ep.ready;
}
static bool f_can(uint16_t len) {
    (void)len;
    return ep.room > 0;
}
static void f_send(const uint8_t *frame, uint16_t len) {
    ep.room--;
    ep.len[ep.n_sent] = len;
    ep.sent[ep.n_sent++] = frame[0];
}

static const net_tx_ops_t ops = {f_ready, f_can, f_send};
static net_txq_t q;
static uint8_t frame[1514];

/* A frame of len bytes whose first byte is id. */
static const uint8_t *f(uint8_t id, uint16_t len) {
    (void)len; /* the endpoint is told the length, not the frame */
    memset(frame, 0xA5, sizeof(frame));
    frame[0] = id;
    return frame;
}

#define ACK_LEN 54u /* an Ethernet frame carrying a bare TCP acknowledgement */

void setUp(void) {
    memset(&ep, 0, sizeof(ep));
    net_tx_flush(&q);
    ep.ready = true;
    ep.room = 100;
}
void tearDown(void) {}

/* An idle endpoint takes the frame at once, and nothing is held. */
void test_WEB_NET_05_sent_at_once_when_the_endpoint_is_free(void) {
    TEST_ASSERT_EQUAL(NET_TX_SENT, net_tx_offer(&q, &ops, f(1, ACK_LEN), ACK_LEN));
    TEST_ASSERT_EQUAL(1, ep.n_sent);
    TEST_ASSERT_EQUAL(ACK_LEN, ep.len[0]);
    TEST_ASSERT_EQUAL(0, net_tx_held(&q));
}

/* A busy endpoint: the frame is copied and held, not dropped, and goes out
 * whole when the endpoint frees, though the caller's buffer has changed. */
void test_WEB_NET_05_a_busy_endpoint_holds_a_copy_of_the_frame(void) {
    ep.room = 0;
    TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, f(7, ACK_LEN), ACK_LEN));
    TEST_ASSERT_EQUAL(1, net_tx_held(&q));
    memset(frame, 0, sizeof(frame));
    ep.room = 1;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(1, ep.n_sent);
    TEST_ASSERT_EQUAL_UINT8(7, ep.sent[0]);
    TEST_ASSERT_EQUAL(ACK_LEN, ep.len[0]);
    TEST_ASSERT_EQUAL(0, net_tx_held(&q));
}

/* Frames leave in the order they were offered: a new one waits behind those
 * held even when the endpoint could take it. */
void test_WEB_NET_05_order_is_kept(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, f(1, ACK_LEN), ACK_LEN);
    net_tx_offer(&q, &ops, f(2, 60), 60);
    ep.room = 1;
    TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, f(3, ACK_LEN), ACK_LEN));
    ep.room = 10;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(3, ep.n_sent);
    TEST_ASSERT_EQUAL_UINT8(1, ep.sent[0]);
    TEST_ASSERT_EQUAL_UINT8(2, ep.sent[1]);
    TEST_ASSERT_EQUAL_UINT8(3, ep.sent[2]);
    TEST_ASSERT_EQUAL(60, ep.len[1]);
}

/* The drain sends what the endpoint takes and keeps the rest. */
void test_WEB_NET_05_drain_stops_when_the_endpoint_is_busy(void) {
    ep.room = 0;
    for (uint8_t i = 1; i <= 3; i++)
        net_tx_offer(&q, &ops, f(i, ACK_LEN), ACK_LEN);
    ep.room = 2;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(2, ep.n_sent);
    TEST_ASSERT_EQUAL(1, net_tx_held(&q));
    ep.room = 1;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL_UINT8(3, ep.sent[2]);
}

/* A frame that does not fit in what is left of the 256 bytes is refused, and
 * what is held stays held: a full segment never fits, and TCP sends it again. */
void test_WEB_NET_05_a_frame_that_does_not_fit_is_refused(void) {
    ep.room = 0;
    TEST_ASSERT_EQUAL(NET_TX_FULL, net_tx_offer(&q, &ops, f(1, 1514), 1514));
    int held = 0;
    while (net_tx_offer(&q, &ops, f((uint8_t)(10 + held), ACK_LEN), ACK_LEN) == NET_TX_HELD)
        held++;
    TEST_ASSERT_TRUE_MESSAGE(held * (int)ACK_LEN <= NET_TXQ_BYTES, "held frames stay within 256 bytes");
    TEST_ASSERT_TRUE(held >= 4);
    TEST_ASSERT_EQUAL(held, net_tx_held(&q));
    ep.room = 100;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(held, ep.n_sent);
    TEST_ASSERT_EQUAL_UINT8(10, ep.sent[0]);
}

/* Room a drain frees is used again, however the frames came and went. */
void test_WEB_NET_05_room_is_used_again(void) {
    for (int round = 0; round < 50; round++) {
        ep.room = 0;
        TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, f((uint8_t)round, 100), 100));
        TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, f((uint8_t)(round + 100), ACK_LEN), ACK_LEN));
        ep.room = 2;
        net_tx_drain(&q, &ops);
        TEST_ASSERT_EQUAL(0, net_tx_held(&q));
    }
    ep.room = 1000;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(100, ep.n_sent);
    TEST_ASSERT_EQUAL(0, net_tx_held(&q));
}

/* The host let the device go: whatever is held goes with it, and nothing new
 * is taken until it is back. */
void test_WEB_NET_05_held_frames_are_dropped_when_the_host_lets_go(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, f(1, ACK_LEN), ACK_LEN);
    ep.ready = false;
    TEST_ASSERT_EQUAL(NET_TX_NOT_READY, net_tx_offer(&q, &ops, f(2, ACK_LEN), ACK_LEN));
    TEST_ASSERT_EQUAL(0, net_tx_held(&q));
    ep.ready = true;
    ep.room = 10;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(0, ep.n_sent);
}

void test_WEB_NET_05_flush_drops_everything(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, f(1, ACK_LEN), ACK_LEN);
    net_tx_offer(&q, &ops, f(2, ACK_LEN), ACK_LEN);
    net_tx_flush(&q);
    TEST_ASSERT_EQUAL(0, net_tx_held(&q));
    ep.room = 10;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(0, ep.n_sent);
}

/* ── Received frames ──────────────────────────────────────────────── */

#define ETH_FRAME_MAX 1514u
static const uint8_t RX_BYTES[ETH_FRAME_MAX] = {1, 2, 3};
static net_rx_slot_t slot;

/* Taken: the slot points at the endpoint's own buffer until the stack has
 * read it, and TinyUSB waits for the renew meanwhile. */
void test_WEB_NET_07_a_frame_is_held_until_the_stack_takes_it(void) {
    memset(&slot, 0, sizeof(slot));
    TEST_ASSERT_TRUE(net_rx_take(&slot, RX_BYTES, 60, ETH_FRAME_MAX));
    TEST_ASSERT_EQUAL_PTR(RX_BYTES, slot.frame);
    TEST_ASSERT_EQUAL(60, slot.len);
    net_rx_done(&slot);
    TEST_ASSERT_NULL(slot.frame);
    TEST_ASSERT_TRUE(net_rx_take(&slot, RX_BYTES, 60, ETH_FRAME_MAX));
}

void test_WEB_NET_07_an_empty_frame_is_handed_back(void) {
    memset(&slot, 0, sizeof(slot));
    TEST_ASSERT_FALSE(net_rx_take(&slot, RX_BYTES, 0, ETH_FRAME_MAX));
    TEST_ASSERT_NULL(slot.frame);
}

void test_WEB_NET_07_a_second_frame_while_one_is_held_is_handed_back(void) {
    memset(&slot, 0, sizeof(slot));
    TEST_ASSERT_TRUE(net_rx_take(&slot, RX_BYTES, 60, ETH_FRAME_MAX));
    TEST_ASSERT_FALSE(net_rx_take(&slot, RX_BYTES + 1, 70, ETH_FRAME_MAX));
    TEST_ASSERT_EQUAL_PTR(RX_BYTES, slot.frame);
    TEST_ASSERT_EQUAL(60, slot.len);
}

void test_WEB_NET_07_a_frame_longer_than_an_ethernet_frame_is_handed_back(void) {
    memset(&slot, 0, sizeof(slot));
    TEST_ASSERT_FALSE(net_rx_take(&slot, RX_BYTES, ETH_FRAME_MAX + 1, ETH_FRAME_MAX));
    TEST_ASSERT_NULL(slot.frame);
}

/* What may be copied into the endpoint's buffer: the whole frame, or nothing. */
void test_WEB_NET_07_a_frame_longer_than_the_endpoint_buffer_is_not_copied(void) {
    TEST_ASSERT_EQUAL(1514, net_tx_copy_len(1514, 1514));
    TEST_ASSERT_EQUAL(0, net_tx_copy_len(1515, 1514));
    TEST_ASSERT_EQUAL(54, net_tx_copy_len(54, 1514));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WEB_NET_05_sent_at_once_when_the_endpoint_is_free);
    RUN_TEST(test_WEB_NET_05_a_busy_endpoint_holds_a_copy_of_the_frame);
    RUN_TEST(test_WEB_NET_05_order_is_kept);
    RUN_TEST(test_WEB_NET_05_drain_stops_when_the_endpoint_is_busy);
    RUN_TEST(test_WEB_NET_05_a_frame_that_does_not_fit_is_refused);
    RUN_TEST(test_WEB_NET_05_room_is_used_again);
    RUN_TEST(test_WEB_NET_05_held_frames_are_dropped_when_the_host_lets_go);
    RUN_TEST(test_WEB_NET_05_flush_drops_everything);
    RUN_TEST(test_WEB_NET_07_a_frame_is_held_until_the_stack_takes_it);
    RUN_TEST(test_WEB_NET_07_an_empty_frame_is_handed_back);
    RUN_TEST(test_WEB_NET_07_a_second_frame_while_one_is_held_is_handed_back);
    RUN_TEST(test_WEB_NET_07_a_frame_longer_than_an_ethernet_frame_is_handed_back);
    RUN_TEST(test_WEB_NET_07_a_frame_longer_than_the_endpoint_buffer_is_not_copied);
    return UNITY_END();
}
