/*
 * HTTP over smallest_tcp (http_transport.c), against the real stack at both
 * ends: a second instance of it is the browser. Frames pass between the two
 * in memory; the board's leave through the USB glue's queue (net_txq.c), as
 * on the board, so a busy endpoint can be played. The exchanges are a small
 * pool served by http_conn.c with a download and an upload route.
 *
 * Verifies [WEB-HTTP-01, WEB-HTTP-02, WEB-HTTP-03, WEB-NET-05, WEB-API-08,
 * WEB-API-13].
 */
#include "unity.h"
#include "http_transport.h"
#include "net_txq.h"
#include "tcp_buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MAX 1514u
#define WIRE_FRAMES 64

/* ── The wire: frames each way, delivered on the next step ────────── */

typedef struct {
    uint8_t buf[WIRE_FRAMES][FRAME_MAX];
    uint16_t len[WIRE_FRAMES];
    int n;
} wire_t;

static wire_t to_board, to_client;

static void wire_put(wire_t *w, const uint8_t *frame, uint16_t len) {
    TEST_ASSERT_TRUE_MESSAGE(w->n < WIRE_FRAMES, "the wire overflowed");
    memcpy(w->buf[w->n], frame, len);
    w->len[w->n++] = len;
}

static void wire_deliver(wire_t *w, net_t *to) {
    static uint8_t frame[FRAME_MAX];
    int n = w->n;
    w->n = 0;
    for (int i = 0; i < n; i++) {
        memcpy(frame, w->buf[i], w->len[i]);
        eth_input(to, frame, w->len[i]);
    }
}

/* ── The board: the stack, and its endpoint behind the glue's queue ── */

static const uint8_t BOARD_MAC[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x01};
static const uint8_t CLIENT_MAC[6] = {0x02, 0x02, 0x84, 0x00, 0x6A, 0x00};
#define BOARD_IP NET_IPV4(192, 168, 9, 1)
#define CLIENT_IP NET_IPV4(192, 168, 9, 2)

static net_t board, client;
static uint8_t board_rx[FRAME_MAX], board_tx[FRAME_MAX], client_rx[FRAME_MAX], client_tx[FRAME_MAX];
static net_txq_t txq;
static int endpoint_busy_every; /* 0: never; n: every nth frame finds the endpoint busy */
static int endpoint_offers;
static int board_data_segments;

static bool ep_ready(void) {
    return true;
}
static bool ep_can_send(uint16_t len) {
    (void)len;
    return endpoint_busy_every == 0 || ++endpoint_offers % endpoint_busy_every != 0;
}
static void ep_send(const uint8_t *frame, uint16_t len) {
    if (len > 54u + 6u) /* more than a bare acknowledgement and its padding */
        board_data_segments++;
    wire_put(&to_client, frame, len);
}
static const net_tx_ops_t ep_ops = {ep_ready, ep_can_send, ep_send};

static int board_send(void *ctx, const uint8_t *frame, uint16_t len) {
    (void)ctx;
    net_tx_result_t r = net_tx_offer(&txq, &ep_ops, frame, len);
    return r == NET_TX_SENT || r == NET_TX_HELD ? len : 0;
}

static int client_send(void *ctx, const uint8_t *frame, uint16_t len) {
    (void)ctx;
    wire_put(&to_board, frame, len);
    return len;
}

static int mac_init(void *ctx) {
    (void)ctx;
    return 0;
}
static int mac_poll(void *ctx) {
    (void)ctx;
    return 0;
}
static int mac_peek(void *ctx, uint16_t offset, uint8_t *buf, uint16_t len) {
    (void)ctx;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}
static void mac_discard(void *ctx) {
    (void)ctx;
}
static const net_mac_t board_mac = {mac_init, board_send, mac_poll, mac_peek, mac_discard, mac_discard};
static const net_mac_t client_mac = {mac_init, client_send, mac_poll, mac_peek, mac_discard, mac_discard};

/* ── The server's exchanges: a pool served by http_conn.c ─────────── */

#define EXCHANGES 4

static struct {
    http_conn_t hc;
    bool in_use;
    uint32_t sent;  /* /blob: body bytes produced */
    uint32_t taken; /* /sink: body bytes taken */
} x[EXCHANGES];
static int gives, wakes;
static int let_go_slot = -1; /* this exchange holds the filesystem the log now needs */
static int sink_take_max = 0xFFFF;

static int slot(const http_conn_t *hc) {
    for (int i = 0; i < EXCHANGES; i++)
        if (&x[i].hc == hc)
            return i;
    TEST_FAIL_MESSAGE("not one of the exchanges");
    return -1;
}

static http_conn_t *x_take(void) {
    for (int i = 0; i < EXCHANGES; i++) {
        if (!x[i].in_use) {
            x[i].in_use = true;
            x[i].sent = x[i].taken = 0;
            http_conn_init(&x[i].hc);
            return &x[i].hc;
        }
    }
    return NULL;
}
static void x_give_back(http_conn_t *hc) {
    x[slot(hc)].in_use = false;
    gives++;
}
static bool x_held(const http_conn_t *hc) {
    (void)hc;
    return false;
}
static bool x_must_let_go(const http_conn_t *hc) {
    return slot(hc) == let_go_slot;
}
static void x_wake(void) {
    wakes++;
}
static const http_exchanges_t exchanges = {x_take, x_give_back, x_held, x_must_let_go, x_wake};

static uint8_t pattern(uint32_t i) {
    return (uint8_t)('a' + i % 26u);
}

static void on_head(http_conn_t *hc) {
    if (strncmp(hc->path, "/blob?n=", 8) == 0)
        http_respond_stream(hc, 200, "application/octet-stream", (uint32_t)strtoul(hc->path + 8, NULL, 10), NULL);
    else if (strcmp(hc->path, "/sink") == 0)
        http_stream(hc);
    else
        http_respond_str(hc, 404, "text/plain", "Not found");
}

static uint16_t on_body(http_conn_t *hc, const uint8_t *data, uint16_t len) {
    int i = slot(hc);
    uint16_t n = len < sink_take_max ? len : (uint16_t)sink_take_max;
    for (uint16_t k = 0; k < n; k++)
        TEST_ASSERT_EQUAL_UINT8(pattern(x[i].taken + k), data[k]);
    x[i].taken += n;
    return n;
}

static bool on_complete(http_conn_t *hc) {
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"bytes\":%lu}", (unsigned long)x[slot(hc)].taken);
    http_respond(hc, 200, "application/json", body, (uint32_t)n);
    return true;
}

static uint16_t fill(http_conn_t *hc, uint8_t *dst, uint16_t max) {
    int i = slot(hc);
    for (uint16_t k = 0; k < max; k++)
        dst[k] = pattern(x[i].sent + k);
    x[i].sent += max;
    return max;
}

static const http_handlers_t handlers = {"", on_head, on_body, on_complete, fill};

/* ── The browser: requests over the second stack ──────────────────── */

#define CLIENTS 12
#define CLIENT_RX 8192
#define CLIENT_TX 2048
#define RESPONSE_MAX (256 * 1024)

typedef struct {
    tcp_conn_t tcp;
    tcp_saw_tx_ctx_t txc;
    tcp_saw_rx_ctx_t rxc;
    uint8_t txb[CLIENT_TX], rxb[CLIENT_RX];
    const uint8_t *req;
    uint32_t req_len, req_sent;
    uint8_t *resp;
    uint32_t resp_len;
    bool closed_ours;
} browser_t;

static browser_t b[CLIENTS];
static tcp_conn_t *client_table[CLIENTS];
static uint8_t requests[CLIENTS][64 * 1024];

static void browser_open(int i, const char *head, uint32_t body_len) {
    browser_t *c = &b[i];
    tcp_saw_tx_init(&c->txc, c->txb, sizeof(c->txb));
    tcp_saw_rx_init(&c->rxc, c->rxb, sizeof(c->rxb));
    tcp_conn_init(&c->tcp, &tcp_saw_tx_ops, &c->txc, &tcp_saw_rx_ops, &c->rxc, NULL);
    uint32_t n = (uint32_t)strlen(head);
    memcpy(requests[i], head, n);
    for (uint32_t k = 0; k < body_len; k++)
        requests[i][n + k] = pattern(k);
    c->req = requests[i];
    c->req_len = n + body_len;
    c->req_sent = 0;
    if (!c->resp)
        c->resp = malloc(RESPONSE_MAX);
    c->resp_len = 0;
    c->closed_ours = false;
    TEST_ASSERT_EQUAL(NET_OK, tcp_connect(&client, &c->tcp, BOARD_IP, BOARD_MAC, 80, (uint16_t)(40000 + i)));
}

static void browser_pump(browser_t *c) {
    tcp_state_t st = tcp_status(&c->tcp);
    if ((st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) && c->req_sent < c->req_len) {
        uint32_t left = c->req_len - c->req_sent;
        int n = tcp_write(&c->tcp, c->req + c->req_sent, (uint16_t)(left < 1460u ? left : 1460u));
        if (n > 0) {
            c->req_sent += (uint32_t)n;
            tcp_output(&client, &c->tcp);
        }
    }
    uint16_t k;
    while ((k = tcp_recv(&c->tcp, c->resp + c->resp_len,
                         (uint16_t)(RESPONSE_MAX - c->resp_len < 1460u ? RESPONSE_MAX - c->resp_len : 1460u))) > 0) {
        c->resp_len += k;
        tcp_window_update(&client, &c->tcp);
    }
    if (st == TCP_CLOSE_WAIT && !c->closed_ours) {
        c->closed_ours = true;
        tcp_close(&client, &c->tcp);
    }
}

/* ── One step: a millisecond of both ends ─────────────────────────── */

static uint32_t now_ms;

static void step(void) {
    wire_deliver(&to_board, &board);
    net_tx_drain(&txq, &ep_ops);
    http_transport_pass(now_ms);
    for (int i = 0; i < EXCHANGES; i++)
        if (x[i].in_use && http_conn_wants_service(&x[i].hc))
            http_conn_service(&x[i].hc, &handlers);
    net_tx_drain(&txq, &ep_ops);
    wire_deliver(&to_client, &client);
    for (int i = 0; i < CLIENTS; i++)
        browser_pump(&b[i]);
    now_ms++;
    net_tick(&board, 1);
    net_tick(&client, 1);
}

static bool all_done(int n) {
    for (int i = 0; i < n; i++) {
        tcp_state_t st = tcp_status(&b[i].tcp);
        if (st != TCP_CLOSED && st != TCP_TIME_WAIT)
            return false;
    }
    return true;
}

static void run_until_done(int n, uint32_t max_ms) {
    uint32_t end = now_ms + max_ms;
    while (!all_done(n) && now_ms < end)
        step();
    TEST_ASSERT_TRUE_MESSAGE(all_done(n), "the exchanges did not finish in time");
}

/* The response's body, after its header block, checked against its length. */
static const uint8_t *body_of(const browser_t *c, uint32_t *len) {
    c->resp[c->resp_len < RESPONSE_MAX ? c->resp_len : RESPONSE_MAX - 1] = 0;
    const char *end = strstr((const char *)c->resp, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "no header block");
    const char *cl = strstr((const char *)c->resp, "Content-Length: ");
    TEST_ASSERT_NOT_NULL(cl);
    *len = (uint32_t)strtoul(cl + 16, NULL, 10);
    const uint8_t *body = (const uint8_t *)end + 4;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(*len, c->resp_len - (uint32_t)(body - c->resp), "framed by Content-Length");
    return body;
}

static void check_blob(int i, uint32_t n) {
    TEST_ASSERT_EQUAL_MEMORY("HTTP/1.1 200", b[i].resp, 12);
    uint32_t len;
    const uint8_t *body = body_of(&b[i], &len);
    TEST_ASSERT_EQUAL_UINT32(n, len);
    for (uint32_t k = 0; k < n; k++)
        if (body[k] != pattern(k))
            TEST_FAIL_MESSAGE("a body byte is wrong");
}

static void stack_init(net_t *n, uint8_t *rx, uint8_t *tx, const uint8_t *mac, const net_mac_t *ops, uint32_t ip) {
    TEST_ASSERT_EQUAL(NET_OK, stcp_net_init(n, rx, FRAME_MAX, tx, FRAME_MAX, mac, ops, NULL)); /* net_stcp.h */
    n->ipv4_addr = ip;
    n->subnet_mask = NET_IPV4(255, 255, 255, 0);
    n->gateway_ipv4 = 0;
}

void setUp(void) {
    memset(&to_board, 0, sizeof(to_board));
    memset(&to_client, 0, sizeof(to_client));
    for (int i = 0; i < EXCHANGES; i++)
        x[i].in_use = false;
    net_tx_flush(&txq);
    endpoint_busy_every = endpoint_offers = board_data_segments = 0;
    gives = wakes = 0;
    let_go_slot = -1;
    sink_take_max = 0xFFFF;
    now_ms = 1000;
    stack_init(&board, board_rx, board_tx, BOARD_MAC, &board_mac, BOARD_IP);
    stack_init(&client, client_rx, client_tx, CLIENT_MAC, &client_mac, CLIENT_IP);
    for (int i = 0; i < CLIENTS; i++) {
        tcp_conn_init(&b[i].tcp, &tcp_saw_tx_ops, &b[i].txc, &tcp_saw_rx_ops, &b[i].rxc, NULL);
        client_table[i] = &b[i].tcp;
    }
    tcp_set_connections(&client, client_table, CLIENTS);
    http_transport_init(&board, &exchanges);
}

void tearDown(void) {}

/* [WEB-HTTP-01, WEB-HTTP-02] A request answered whole, framed by
 * Content-Length, and the connection closed by the board. */
void test_WEB_HTTP_02_a_request_is_answered_and_closed(void) {
    const http_transport_counts_t *before = http_transport_counts();
    uint32_t accepts = before->accepts, served = before->served;
    browser_open(0, "GET /blob?n=100 HTTP/1.1\r\nHost: pyro.local\r\n\r\n", 0);
    run_until_done(1, 5000);
    check_blob(0, 100);
    TEST_ASSERT_NOT_NULL(strstr((const char *)b[0].resp, "Connection: close"));
    TEST_ASSERT_EQUAL(1, gives);
    TEST_ASSERT_EQUAL_UINT32(accepts + 1, http_transport_counts()->accepts);
    TEST_ASSERT_EQUAL_UINT32(served + 1, http_transport_counts()->served);
}

/* [WEB-HTTP-01] A download fifty times the tx ring arrives whole, in full
 * segments: one in flight at a time, each refilled before it is sent. */
void test_WEB_HTTP_01_a_download_larger_than_the_ring_arrives_whole(void) {
    const uint32_t n = 100000;
    browser_open(0, "GET /blob?n=100000 HTTP/1.1\r\nHost: pyro.local\r\n\r\n", 0);
    run_until_done(1, 20000);
    check_blob(0, n);
    char msg[64];
    snprintf(msg, sizeof(msg), "%d data segments for %lu bytes", board_data_segments, (unsigned long)n);
    TEST_ASSERT_TRUE_MESSAGE(board_data_segments <= (int)(n / 1460u) + 3, msg);
}

/* [WEB-HTTP-03] An upload the handler takes 64 bytes at a time: the window
 * follows the rx ring, so the browser is held back and never has a segment
 * refused and sent again. */
void test_WEB_HTTP_03_an_upload_is_read_only_as_fast_as_it_is_taken(void) {
    sink_take_max = 64;
    browser_open(0, "POST /sink HTTP/1.1\r\nHost: pyro.local\r\nContent-Length: 20000\r\n\r\n", 20000);
    run_until_done(1, 60000);
    TEST_ASSERT_EQUAL_MEMORY("HTTP/1.1 200", b[0].resp, 12);
    TEST_ASSERT_NOT_NULL(strstr((const char *)b[0].resp, "{\"bytes\":20000}"));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, b[0].tcp.retransmits, "the browser sent nothing again");
}

/* Nine browsers at once against four exchanges: the links waiting for an
 * exchange are held by their shut windows, and every one is served. */
void test_WEB_HTTP_01_more_browsers_than_exchanges_are_all_served(void) {
    uint32_t waited = http_transport_counts()->conn_full;
    for (int i = 0; i < 9; i++)
        browser_open(i, "GET /blob?n=5000 HTTP/1.1\r\nHost: pyro.local\r\n\r\n", 0);
    run_until_done(9, 30000);
    for (int i = 0; i < 9; i++)
        check_blob(i, 5000);
    TEST_ASSERT_TRUE(http_transport_counts()->conn_full > waited);
}

/* [WEB-NET-05] An endpoint busy for every third frame: a refused segment
 * goes again at the next pass, a refused acknowledgement waits in the queue,
 * and the download still arrives whole and soon. */
void test_WEB_NET_05_a_busy_endpoint_loses_nothing(void) {
    endpoint_busy_every = 3;
    browser_open(0, "GET /blob?n=30000 HTTP/1.1\r\nHost: pyro.local\r\n\r\n", 0);
    uint32_t start = now_ms;
    run_until_done(1, 20000);
    check_blob(0, 30000);
    TEST_ASSERT_TRUE_MESSAGE(now_ms - start < 2000, "no retransmission timeout was waited for");
}

/* [WEB-NET-05] The same during an upload the handler takes slowly: the
 * board's acknowledgements and window updates are what a busy endpoint would
 * lose, and the stack does not send those again, so they wait in the queue;
 * the upload takes no longer than on an idle endpoint. */
static uint32_t upload_ms(int busy_every) {
    setUp();
    endpoint_busy_every = busy_every;
    sink_take_max = 64;
    browser_open(0, "POST /sink HTTP/1.1\r\nHost: pyro.local\r\nContent-Length: 20000\r\n\r\n", 20000);
    uint32_t start = now_ms;
    run_until_done(1, 60000);
    TEST_ASSERT_NOT_NULL(strstr((const char *)b[0].resp, "{\"bytes\":20000}"));
    return now_ms - start;
}

void test_WEB_NET_05_a_busy_endpoint_holds_up_no_upload(void) {
    uint32_t idle = upload_ms(0), busy = upload_ms(3);
    char msg[64];
    snprintf(msg, sizeof(msg), "%lu ms on an idle endpoint, %lu ms on a busy one", (unsigned long)idle,
             (unsigned long)busy);
    printf("  %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(busy < idle * 2u + 100u, msg);
}

/* [WEB-API-08, DD-058] An exchange holding the filesystem when the flight
 * log needs it lets go at once: the link is aborted and the exchange given
 * back. */
void test_WEB_API_08_an_exchange_holding_the_filesystem_lets_go(void) {
    sink_take_max = 0;
    browser_open(0, "POST /sink HTTP/1.1\r\nHost: pyro.local\r\nContent-Length: 5000\r\n\r\n", 5000);
    for (int i = 0; i < 200; i++)
        step();
    TEST_ASSERT_TRUE(x[0].in_use);
    uint32_t errs = http_transport_counts()->http_err;
    let_go_slot = 0;
    for (int i = 0; i < 50; i++)
        step();
    TEST_ASSERT_FALSE(x[0].in_use);
    TEST_ASSERT_EQUAL(1, gives);
    TEST_ASSERT_EQUAL_UINT32(errs + 1, http_transport_counts()->http_err);
    TEST_ASSERT_EQUAL_INT(TCP_CLOSED, tcp_status(&b[0].tcp));
}

/* A browser that connects and says nothing is let go after HTTP_IDLE_MS. */
void test_an_idle_link_is_let_go(void) {
    browser_open(0, "", 0);
    uint32_t idle = http_transport_counts()->idle_aborts;
    for (uint32_t t = 0; t < HTTP_IDLE_MS - 500u; t++)
        step();
    TEST_ASSERT_EQUAL_INT(TCP_ESTABLISHED, tcp_status(&b[0].tcp));
    for (int t = 0; t < 1000; t++)
        step();
    TEST_ASSERT_EQUAL_INT(TCP_CLOSED, tcp_status(&b[0].tcp));
    TEST_ASSERT_EQUAL_UINT32(idle + 1, http_transport_counts()->idle_aborts);
}

/* [WEB-API-13] Every link is counted in its TCP state. */
void test_WEB_API_13_links_are_counted_by_state(void) {
    uint16_t states[11];
    uint32_t nrtx;
    http_transport_states(states, &nrtx);
    TEST_ASSERT_EQUAL_UINT16(HTTP_LINKS, states[TCP_LISTEN]);
    browser_open(0, "GET /blob?n=10 HTTP/1.1\r\nHost: pyro.local\r\n\r\n", 0);
    run_until_done(1, 5000);
    for (int i = 0; i < 3000; i++)
        step();
    http_transport_states(states, &nrtx);
    uint32_t total = 0;
    for (int i = 0; i < 11; i++)
        total += states[i];
    TEST_ASSERT_EQUAL_UINT32(HTTP_LINKS, total);
    TEST_ASSERT_EQUAL_UINT32(0, nrtx);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WEB_HTTP_02_a_request_is_answered_and_closed);
    RUN_TEST(test_WEB_HTTP_01_a_download_larger_than_the_ring_arrives_whole);
    RUN_TEST(test_WEB_HTTP_03_an_upload_is_read_only_as_fast_as_it_is_taken);
    RUN_TEST(test_WEB_HTTP_01_more_browsers_than_exchanges_are_all_served);
    RUN_TEST(test_WEB_NET_05_a_busy_endpoint_loses_nothing);
    RUN_TEST(test_WEB_NET_05_a_busy_endpoint_holds_up_no_upload);
    RUN_TEST(test_WEB_API_08_an_exchange_holding_the_filesystem_lets_go);
    RUN_TEST(test_an_idle_link_is_let_go);
    RUN_TEST(test_WEB_API_13_links_are_counted_by_state);
    return UNITY_END();
}
