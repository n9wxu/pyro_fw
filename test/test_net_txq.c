/*
 * The USB network's frames (net_txq.c), against a fake endpoint and a fake
 * buffer pool. Sent: a frame the endpoint cannot take yet is held and sent,
 * in order, as soon as it can [WEB-NET-05, DD-070]. Received: the receive
 * callback says it holds a frame only when it does.
 */
#include "unity.h"
#include "net_txq.h"
#include <string.h>

static struct {
    bool ready;   /* the device is configured */
    int room;     /* frames the endpoint takes before it is busy */
    int sent[64]; /* frame ids, in the order they went out */
    int n_sent;
    int held[64]; /* references held on each frame id */
} ep;

static bool f_ready(void) {
    return ep.ready;
}
static bool f_can(void *frame) {
    (void)frame;
    return ep.room > 0;
}
static void f_send(void *frame) {
    ep.room--;
    ep.sent[ep.n_sent++] = (int)(intptr_t)frame;
}
static void f_hold(void *frame) {
    ep.held[(intptr_t)frame]++;
}
static void f_release(void *frame) {
    ep.held[(intptr_t)frame]--;
}

static const net_tx_ops_t ops = {f_ready, f_can, f_send, f_hold, f_release};
static net_txq_t q;

#define FRAME(i) ((void *)(intptr_t)(i))

void setUp(void) {
    memset(&ep, 0, sizeof(ep));
    memset(&q, 0, sizeof(q));
    ep.ready = true;
    ep.room = 100;
}
void tearDown(void) {}

/* An idle endpoint takes the frame at once, and nothing is held. */
void test_TXQ_01_sent_at_once_when_the_endpoint_is_free(void) {
    TEST_ASSERT_EQUAL(NET_TX_SENT, net_tx_offer(&q, &ops, FRAME(1)));
    TEST_ASSERT_EQUAL(1, ep.n_sent);
    TEST_ASSERT_EQUAL(0, ep.held[1]);
    TEST_ASSERT_EQUAL(0, q.n);
}

/* A busy endpoint: the frame is held, not dropped, and goes out when the
 * endpoint frees, its reference released. */
void test_TXQ_02_a_busy_endpoint_holds_the_frame(void) {
    ep.room = 0;
    TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, FRAME(1)));
    TEST_ASSERT_EQUAL(1, ep.held[1]);
    TEST_ASSERT_EQUAL(0, ep.n_sent);
    ep.room = 1;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(1, ep.n_sent);
    TEST_ASSERT_EQUAL(1, ep.sent[0]);
    TEST_ASSERT_EQUAL(0, ep.held[1]);
    TEST_ASSERT_EQUAL(0, q.n);
}

/* Frames leave in the order they came: a new frame never passes one held,
 * even when the endpoint has room for one just then. */
void test_TXQ_03_order_is_kept(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, FRAME(1));
    net_tx_offer(&q, &ops, FRAME(2));
    ep.room = 1;
    TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, FRAME(3)));
    TEST_ASSERT_EQUAL(1, ep.n_sent);
    ep.room = 100;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(3, ep.n_sent);
    TEST_ASSERT_EQUAL_INT_ARRAY(((int[]){1, 2, 3}), ep.sent, 3);
}

/* The drain sends only what the endpoint takes, and keeps the rest. */
void test_TXQ_04_drain_stops_when_the_endpoint_is_busy(void) {
    ep.room = 0;
    for (int i = 1; i <= 4; i++)
        net_tx_offer(&q, &ops, FRAME(i));
    ep.room = 2;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(2, ep.n_sent);
    TEST_ASSERT_EQUAL(2, q.n);
    ep.room = 2;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL_INT_ARRAY(((int[]){1, 2, 3, 4}), ep.sent, 4);
}

/* Full: the frame is refused, not held, and TCP's retransmission resends it. */
void test_TXQ_05_full_refuses(void) {
    ep.room = 0;
    for (int i = 1; i <= NET_TXQ_N; i++)
        TEST_ASSERT_EQUAL(NET_TX_HELD, net_tx_offer(&q, &ops, FRAME(i)));
    TEST_ASSERT_EQUAL(NET_TX_FULL, net_tx_offer(&q, &ops, FRAME(NET_TXQ_N + 1)));
    TEST_ASSERT_EQUAL(0, ep.held[NET_TXQ_N + 1]);
    TEST_ASSERT_EQUAL(NET_TXQ_N, q.n);
}

/* A device the host has let go takes nothing, and what was held is released:
 * the frames are for a link that is gone. */
void test_TXQ_06_not_ready_releases_everything(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, FRAME(1));
    net_tx_offer(&q, &ops, FRAME(2));
    ep.ready = false;
    TEST_ASSERT_EQUAL(NET_TX_NOT_READY, net_tx_offer(&q, &ops, FRAME(3)));
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(0, q.n);
    TEST_ASSERT_EQUAL(0, ep.held[1]);
    TEST_ASSERT_EQUAL(0, ep.held[2]);
    TEST_ASSERT_EQUAL(0, ep.n_sent);
}

/* Round and round the ring: many more frames than it holds, all in order. */
void test_TXQ_07_wraps(void) {
    int next = 1;
    for (int round = 0; round < 10; round++) {
        ep.room = 0;
        for (int i = 0; i < 3; i++)
            net_tx_offer(&q, &ops, FRAME(next++));
        ep.room = 100;
        net_tx_drain(&q, &ops);
    }
    TEST_ASSERT_EQUAL(30, ep.n_sent);
    for (int i = 0; i < 30; i++)
        TEST_ASSERT_EQUAL(i + 1, ep.sent[i]);
}

/* The host re-initialised the interface: what waited was for the old link. */
void test_TXQ_08_flush_releases_everything(void) {
    ep.room = 0;
    net_tx_offer(&q, &ops, FRAME(1));
    net_tx_offer(&q, &ops, FRAME(2));
    net_tx_flush(&q, &ops);
    TEST_ASSERT_EQUAL(0, q.n);
    TEST_ASSERT_EQUAL(0, ep.held[1] + ep.held[2]);
    ep.room = 100;
    net_tx_drain(&q, &ops);
    TEST_ASSERT_EQUAL(0, ep.n_sent);
}

/* ── Received frames ──────────────────────────────────────────────── */

static struct {
    int pool;      /* buffers left */
    int allocated; /* buffers out */
    bool fill_fails;
    uint16_t cap; /* the largest buffer the pool gives */
    uint8_t buf[2048];
    uint16_t len;
} rx;

static void *rx_alloc(uint16_t size) {
    if (rx.pool == 0 || size > rx.cap)
        return NULL;
    rx.pool--;
    rx.allocated++;
    return rx.buf;
}
static bool rx_fill(void *frame, const uint8_t *src, uint16_t size) {
    if (rx.fill_fails)
        return false;
    memcpy(frame, src, size);
    rx.len = size;
    return true;
}
static void rx_release(void *frame) {
    (void)frame;
    rx.pool++;
    rx.allocated--;
}

static const net_rx_ops_t rx_ops = {rx_alloc, rx_fill, rx_release};
static const uint8_t FRAME_BYTES[64] = {1, 2, 3};

static void rx_reset(void) {
    memset(&rx, 0, sizeof(rx));
    rx.pool = 4;
    rx.cap = 1514;
}

/* Taken: the slot holds a copy until net_service() hands it to lwIP. */
void test_RXQ_01_a_frame_is_held_until_lwip_takes_it(void) {
    rx_reset();
    void *slot = NULL;
    TEST_ASSERT_TRUE(net_rx_take(&slot, &rx_ops, FRAME_BYTES, 60));
    TEST_ASSERT_NOT_NULL(slot);
    TEST_ASSERT_EQUAL_UINT16(60, rx.len);
    TEST_ASSERT_EQUAL_MEMORY(FRAME_BYTES, rx.buf, 60);
}

/* CR-14: TinyUSB waits for tud_network_recv_renew() after a true, and
 * net_service() renews only once it has a frame to give lwIP. A true with
 * nothing held stopped USB reception for good. */
void test_RXQ_02_with_no_buffer_the_frame_is_handed_back(void) {
    rx_reset();
    rx.pool = 0;
    void *slot = NULL;
    TEST_ASSERT_FALSE_MESSAGE(net_rx_take(&slot, &rx_ops, FRAME_BYTES, 60), "true here stalls reception");
    TEST_ASSERT_NULL(slot);
}

void test_RXQ_03_an_empty_frame_is_handed_back(void) {
    rx_reset();
    void *slot = NULL;
    TEST_ASSERT_FALSE(net_rx_take(&slot, &rx_ops, FRAME_BYTES, 0));
    TEST_ASSERT_NULL(slot);
    TEST_ASSERT_EQUAL(0, rx.allocated);
}

void test_RXQ_04_a_frame_that_will_not_copy_is_handed_back_and_its_buffer_freed(void) {
    rx_reset();
    rx.fill_fails = true;
    void *slot = NULL;
    TEST_ASSERT_FALSE(net_rx_take(&slot, &rx_ops, FRAME_BYTES, 60));
    TEST_ASSERT_NULL(slot);
    TEST_ASSERT_EQUAL(0, rx.allocated);
}

/* One frame at a time: TinyUSB drops a second while lwIP has the first. */
void test_RXQ_05_a_second_frame_while_one_is_held_is_handed_back(void) {
    rx_reset();
    void *slot = NULL;
    net_rx_take(&slot, &rx_ops, FRAME_BYTES, 60);
    void *held = slot;
    TEST_ASSERT_FALSE(net_rx_take(&slot, &rx_ops, FRAME_BYTES, 60));
    TEST_ASSERT_EQUAL_PTR(held, slot);
    TEST_ASSERT_EQUAL(1, rx.allocated);
}

/* CR-24: the endpoint buffer holds CFG_TUD_NET_MTU bytes, a whole Ethernet
 * frame; a longer frame is not copied over its end. */
void test_RXQ_06_a_frame_longer_than_the_endpoint_buffer_is_not_copied(void) {
    TEST_ASSERT_EQUAL_UINT16(1514, net_tx_copy_len(1514, 1514));
    TEST_ASSERT_EQUAL_UINT16(60, net_tx_copy_len(60, 1514));
    TEST_ASSERT_EQUAL_UINT16(0, net_tx_copy_len(1515, 1514));
    TEST_ASSERT_EQUAL_UINT16(0, net_tx_copy_len(70000, 1514));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_TXQ_01_sent_at_once_when_the_endpoint_is_free);
    RUN_TEST(test_TXQ_02_a_busy_endpoint_holds_the_frame);
    RUN_TEST(test_TXQ_03_order_is_kept);
    RUN_TEST(test_TXQ_04_drain_stops_when_the_endpoint_is_busy);
    RUN_TEST(test_TXQ_05_full_refuses);
    RUN_TEST(test_TXQ_06_not_ready_releases_everything);
    RUN_TEST(test_TXQ_07_wraps);
    RUN_TEST(test_TXQ_08_flush_releases_everything);
    RUN_TEST(test_RXQ_01_a_frame_is_held_until_lwip_takes_it);
    RUN_TEST(test_RXQ_02_with_no_buffer_the_frame_is_handed_back);
    RUN_TEST(test_RXQ_03_an_empty_frame_is_handed_back);
    RUN_TEST(test_RXQ_04_a_frame_that_will_not_copy_is_handed_back_and_its_buffer_freed);
    RUN_TEST(test_RXQ_05_a_second_frame_while_one_is_held_is_handed_back);
    RUN_TEST(test_RXQ_06_a_frame_longer_than_the_endpoint_buffer_is_not_copied);
    return UNITY_END();
}
