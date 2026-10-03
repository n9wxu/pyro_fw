/*
 * The USB network's transmit queue (net_txq.c), against a fake endpoint. A
 * frame the endpoint cannot take yet is held and sent, in order, as soon as it
 * can: dropping it left lwIP to find out by its retransmit timer, 3 s and
 * doubling, with the heap held meanwhile (G4-N: 40 % of frames refused under
 * G4's load, the heap out 27,865 times).
 *
 * Verifies [WEB-NET-05].
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

/* Full: the frame is refused, not held, and lwIP's retransmit has it. */
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
    return UNITY_END();
}
