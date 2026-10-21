/*
 * /api/net's rendering (net_stats.c): the network's counters for the bench,
 * so an HTTP outage can be told apart [WEB-API-13]. Keys, their order and their
 * formatting are the API.
 *
 * Verifies [WEB-API-13].
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
    s->states[1] = 14; /* LISTEN */
    s->states[4] = 1;  /* ESTABLISHED */
    s->states[10] = 1; /* TIME_WAIT */
    s->retransmits = 9;
    s->nrtx_max = 2;
    s->accepts = 4321;
    s->conn_full = 6;
    s->http_err = 8;
    s->idle_aborts = 2;
    s->served = 4300;
    s->rx_frames = 10000;
    s->rx_drops = 3;
    s->tx_sent = 9999;
    s->tx_held = 40;
    s->tx_refused = 11;
    s->usb[0] = 1;
    s->last_accept_ms = 123456;
}

static const char *const EXPECTED = "{\"states\":[0,14,0,0,1,0,0,0,0,0,1],\"retransmits\":9,\"nrtx_max\":2,"
                                    "\"accepts\":4321,\"conn_full\":6,\"http_err\":8,\"idle_aborts\":2,\"served\":4300,"
                                    "\"rx\":[10000,3],\"tx\":[9999,40,11],\"usb\":[1,0,0,0],\"last_accept_ms\":123456}";

/* TCP's connections by state, CLOSED through TIME_WAIT; the segments sent
 * again; what the transport refused; the frames each way. */
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
