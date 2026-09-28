/*
 * /api/net's rendering (net_stats.c): the network's counters for the bench,
 * so an HTTP outage (G4-N) names what ran out. Keys, their order and their
 * formatting are the API.
 */
#include "unity.h"
#include "net_stats.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void typical(net_snap_t *s) {
    memset(s, 0, sizeof(*s));
    s->heap = (net_pool_t){1200, 7968, 3};
    s->tcp_pcb = (net_pool_t){5, 16, 1};
    s->tcp_seg = (net_pool_t){2, 16, 4};
    s->pbuf_pool = (net_pool_t){1, 9, 0};
    s->tcp_xmit = 900;
    s->tcp_recv = 800;
    s->tcp_drop = 2;
    s->tcp_memerr = 5;
    s->icmp_recv = 12;
    s->icmp_xmit = 12;
    s->states[4] = 1;  /* ESTABLISHED */
    s->states[10] = 4; /* TIME_WAIT */
    s->sndq_max = 3;
    s->nrtx_max = 2;
    s->accepts = 4321;
    s->accept_refused = 1;
    s->write_fails = 17;
    s->conn_full = 6;
    s->http_err = 8;
    s->idle_aborts = 2;
    s->rx_frames = 10000;
    s->rx_drops = 3;
    s->tx_sent = 9999;
    s->tx_held = 40;
    s->tx_refused = 11;
    s->usb[0] = 1;
    s->last_accept_ms = 123456;
}

static const char *const EXPECTED =
    "{\"heap\":[1200,7968,3],\"tcp_pcb\":[5,16,1],\"tcp_seg\":[2,16,4],\"pbuf_pool\":[1,9,0],"
    "\"tcp\":{\"xmit\":900,\"recv\":800,\"drop\":2,\"memerr\":5},\"icmp\":[12,12],"
    "\"states\":[0,0,0,0,1,0,0,0,0,0,4],\"sndq_max\":3,\"nrtx_max\":2,"
    "\"accepts\":4321,\"accept_refused\":1,\"write_fails\":17,\"conn_full\":6,\"http_err\":8,\"idle_aborts\":2,"
    "\"rx\":[10000,3],\"tx\":[9999,40,11],\"usb\":[1,0,0,0],\"last_accept_ms\":123456}";

/* Each pool as used, high-water mark and allocation failures; TCP's
 * connections by state, CLOSED through TIME_WAIT. */
void test_NET_01_keys_order_and_formatting_are_the_api(void) {
    net_snap_t s;
    typical(&s);
    char out[NET_JSON_MAX];
    int n = net_json(&s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(EXPECTED, out);
    TEST_ASSERT_EQUAL((int)strlen(EXPECTED), n);
}

/* Every counter at its widest still fits the bound, whole. */
void test_NET_02_the_widest_fits_its_bound(void) {
    net_snap_t s;
    memset(&s, 0xFF, sizeof(s));
    char out[NET_JSON_MAX];
    int n = net_json(&s, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0 && n < NET_JSON_MAX);
    TEST_ASSERT_EQUAL('}', out[n - 1]);
}

/* Short of room, nothing half-written goes out. */
void test_NET_03_too_small_renders_nothing(void) {
    net_snap_t s;
    typical(&s);
    char out[64];
    TEST_ASSERT_EQUAL(0, net_json(&s, out, sizeof(out)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_NET_01_keys_order_and_formatting_are_the_api);
    RUN_TEST(test_NET_02_the_widest_fits_its_bound);
    RUN_TEST(test_NET_03_too_small_renders_nothing);
    return UNITY_END();
}
