/*
 * The HTTP engine against a byte stream.
 *
 * A fake transport feeds each request in chosen pieces -- whole, split at
 * every byte position, byte by byte, at random -- and drains the response
 * through a window that takes a few bytes at a time. The answer must not
 * depend on any of that: TCP is a stream.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/http_conn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Routes under test ────────────────────────────────────────────── */

static uint8_t sink[16384];
static uint32_t sink_len;
/* The sink stands in for the flash window: shut for this many passes, and
 * it only opens between passes. Offered again within a pass that found it
 * shut is a spin the real window could never end. */
static int shut_passes;
static int offers_while_shut;
static int complete_refusals;
static uint32_t stream_len;
static char big[4000];

static void on_head(http_conn_t *c) {
    if (strcmp(c->method, "GET") == 0 && strcmp(c->path, "/hello") == 0) {
        http_respond_str(c, 200, "text/plain", "hi");
    } else if (strcmp(c->method, "GET") == 0 && strcmp(c->path, "/big") == 0) {
        http_respond(c, 200, "text/plain", big, sizeof(big));
    } else if (strcmp(c->method, "GET") == 0 && strcmp(c->path, "/file") == 0) {
        http_respond_stream(c, 200, "application/octet-stream", stream_len, "X-Extra: 1\r\n");
    } else if (strcmp(c->method, "POST") == 0 && strcmp(c->path, "/echo") == 0) {
        http_gather(c, 100);
    } else if (strcmp(c->method, "POST") == 0 && strcmp(c->path, "/upload") == 0) {
        http_stream(c);
    } else if (strcmp(c->method, "POST") == 0 && strcmp(c->path, "/slow") == 0) {
        /* body discarded; on_complete answers */
    } else {
        http_respond_str(c, 404, "text/plain", "Not found");
    }
}

static uint16_t on_body(http_conn_t *c, const uint8_t *data, uint16_t len) {
    (void)c;
    if (shut_passes > 0) {
        offers_while_shut++;
        TEST_ASSERT_EQUAL_MESSAGE(1, offers_while_shut, "offered again in the pass that found the sink shut: a spin");
        return 0;
    }
    /* Takes at most 100 at a time, so the engine must keep offering. */
    uint16_t n = len > 100 ? 100 : len;
    memcpy(sink + sink_len, data, n);
    sink_len += n;
    return n;
}

static bool on_complete(http_conn_t *c) {
    if (complete_refusals > 0) {
        complete_refusals--;
        return false;
    }
    if (strcmp(c->path, "/echo") == 0) {
        http_respond(c, 200, "text/plain", c->work, c->gathered);
    } else {
        char msg[32];
        snprintf(msg, sizeof(msg), "got %lu", (unsigned long)sink_len);
        http_respond_str(c, 201, "text/plain", msg);
    }
    return true;
}

static uint32_t fill_pos;
static uint16_t fill(http_conn_t *c, uint8_t *dst, uint16_t max) {
    (void)c;
    for (uint16_t i = 0; i < max; i++) {
        dst[i] = (uint8_t)('a' + (fill_pos + i) % 26);
    }
    fill_pos += max;
    return max;
}

static const http_handlers_t H = {
    .common_headers = "Access-Control-Allow-Origin: *\r\n",
    .on_head = on_head,
    .on_body = on_body,
    .on_complete = on_complete,
    .fill = fill,
};

/* ── The fake transport ───────────────────────────────────────────── */

static http_conn_t conn;
static char out[65536];
static uint32_t out_len;
static uint32_t consumed_total;

/* Move what the window allows out of tx, as a TCP stack would. */
static void drain(uint16_t window) {
    while (net_ring_readable(&conn.tx) > 0) {
        uint16_t n = net_ring_readable(&conn.tx);
        if (n > window) {
            n = window;
        }
        out_len += net_ring_read(&conn.tx, out + out_len, n);
        if (window < 0xFFFF) {
            break; /* one window's worth per pass */
        }
    }
}

static void pass(uint16_t window) {
    offers_while_shut = 0;
    http_conn_service(&conn, &H);
    if (shut_passes > 0) {
        shut_passes--;
    }
    consumed_total += http_conn_take_consumed(&conn);
    drain(window);
}

/* Feed data in pieces of the given sizes (cycled), servicing between them,
 * then keep servicing until the exchange is done or stalls. */
static void run(const char *data, uint32_t len, const uint16_t *pieces, int npieces, uint16_t window, bool eof) {
    http_conn_init(&conn);
    out_len = 0;
    consumed_total = 0;
    uint32_t off = 0;
    int pi = 0;
    int idle = 0;
    while (!http_conn_done(&conn) && !conn.failed && idle < 20000) {
        uint32_t before = off + out_len;
        if (off < len) {
            uint16_t want = pieces[pi % npieces];
            pi++;
            if (want > len - off) {
                want = (uint16_t)(len - off);
            }
            off += net_ring_write(&conn.rx, data + off, want);
        } else if (eof) {
            conn.rx_eof = true;
        }
        pass(window);
        idle = (off + out_len == before) ? idle + 1 : 0;
    }
    drain(0xFFFF);
}

static void run_whole(const char *req) {
    uint16_t all = 0xFFFF;
    run(req, (uint32_t)strlen(req), &all, 1, 0xFFFF, false);
}

/* The status code, and the body as framed by Content-Length (which must
 * match what actually followed the header block). */
static int status_of(const char *resp) {
    int code = 0;
    const char *p = strstr(resp, "HTTP/1.1 ");
    while (p && strncmp(p, "HTTP/1.1 100 ", 13) == 0) {
        p = strstr(p + 1, "HTTP/1.1 ");
    }
    if (p) {
        sscanf(p, "HTTP/1.1 %d", &code);
    }
    return code;
}

static const char *final_head(const char *resp) {
    const char *p = resp;
    while (strncmp(p, "HTTP/1.1 100 ", 13) == 0) {
        p = strstr(p, "\r\n\r\n") + 4;
    }
    return p;
}

static const char *body_of(const char *resp, uint32_t resp_len, uint32_t *blen) {
    const char *head = final_head(resp);
    const char *end = strstr(head, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "no end of header block");
    const char *cl = strstr(head, "Content-Length: ");
    TEST_ASSERT_TRUE_MESSAGE(cl && cl < end, "every response is framed by Content-Length");
    unsigned long n = strtoul(cl + 16, NULL, 10);
    const char *body = end + 4;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n, (uint32_t)(resp + resp_len - body),
                                     "Content-Length must be exactly what was sent");
    TEST_ASSERT_NOT_NULL(strstr(head, "Connection: close\r\n"));
    *blen = (uint32_t)n;
    return body;
}

void setUp(void) {
    sink_len = 0;
    shut_passes = 0;
    complete_refusals = 0;
    fill_pos = 0;
    stream_len = 5000;
    for (unsigned i = 0; i < sizeof(big); i++) {
        big[i] = (char)('A' + i % 26);
    }
}

void tearDown(void) {}

/* ── Framing ──────────────────────────────────────────────────────── */

static const char ECHO[] = "POST /echo HTTP/1.1\r\nHost: pyro\r\nContent-Length: 11\r\n\r\nhello world";

void test_HTTP_01_whole_request(void) {
    run_whole(ECHO);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));
    uint32_t n;
    const char *b = body_of(out, out_len, &n);
    TEST_ASSERT_EQUAL_STRING_LEN("hello world", b, 11);
    TEST_ASSERT_NOT_NULL(strstr(out, "Access-Control-Allow-Origin: *\r\n"));
    TEST_ASSERT_EQUAL_UINT32(strlen(ECHO), consumed_total);
}

/* The one that broke /api/test_mode on hardware: a body in a later segment.
 * Here, every possible cut. */
void test_HTTP_02_split_at_every_byte_position(void) {
    run_whole(ECHO);
    char want[512];
    uint32_t want_len = out_len;
    memcpy(want, out, out_len);
    for (uint16_t cut = 1; cut < strlen(ECHO); cut++) {
        uint16_t pieces[2] = {cut, 0xFFFF};
        run(ECHO, (uint32_t)strlen(ECHO), pieces, 2, 0xFFFF, false);
        char msg[48];
        snprintf(msg, sizeof(msg), "split at %u", cut);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_len, out_len, msg);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, out, want_len, msg);
    }
}

void test_HTTP_03_byte_by_byte_and_random_pieces(void) {
    run_whole(ECHO);
    char want[512];
    uint32_t want_len = out_len;
    memcpy(want, out, out_len);
    uint16_t one = 1;
    run(ECHO, (uint32_t)strlen(ECHO), &one, 1, 1, false);
    TEST_ASSERT_EQUAL_UINT32(want_len, out_len);
    TEST_ASSERT_EQUAL_MEMORY(want, out, want_len);
    srand(1);
    for (int trial = 0; trial < 200; trial++) {
        uint16_t pieces[8];
        for (int i = 0; i < 8; i++) {
            pieces[i] = (uint16_t)(1 + rand() % 20);
        }
        run(ECHO, (uint32_t)strlen(ECHO), pieces, 8, (uint16_t)(1 + rand() % 9), false);
        TEST_ASSERT_EQUAL_UINT32(want_len, out_len);
        TEST_ASSERT_EQUAL_MEMORY(want, out, want_len);
    }
}

/* A browser's POST headers run past 512 bytes, which is where the old
 * server's copy stopped and read an empty body. */
void test_HTTP_04_long_header_block_and_any_case_length(void) {
    char req[2048];
    int n = snprintf(req, sizeof(req), "POST /echo HTTP/1.1\r\nHost: pyro\r\n");
    for (int i = 0; i < 12; i++) {
        n += snprintf(req + n, sizeof(req) - (size_t)n,
                      "sec-ch-ua-%d: \"Chromium\";v=\"140\", \"Not=A?Brand\";v=\"24\"\r\n", i);
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "content-LENGTH:   5  \r\n\r\nabcde");
    TEST_ASSERT_TRUE(n > 700);
    uint16_t pieces[3] = {300, 700, 13};
    run(req, (uint32_t)n, pieces, 3, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));
    uint32_t bl;
    TEST_ASSERT_EQUAL_STRING_LEN("abcde", body_of(out, out_len, &bl), 5);
}

void test_HTTP_05_leading_empty_lines_are_ignored(void) {
    run_whole("\r\n\r\nGET /hello HTTP/1.1\r\n\r\n");
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));
}

void test_HTTP_06_head_gets_headers_and_no_body(void) {
    run_whole("HEAD /hello HTTP/1.1\r\n\r\n");
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "Content-Length: 2\r\n"));
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n", out + out_len - 4);
}

/* One request per connection: a second one behind it is not answered. */
void test_HTTP_07_second_request_in_the_same_read_is_not_answered(void) {
    run_whole("GET /hello HTTP/1.1\r\n\r\nGET /hello HTTP/1.1\r\n\r\n");
    out[out_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(out, "hi"));
    TEST_ASSERT_NULL(strstr(strstr(out, "hi") + 2, "HTTP/1.1"));
}

/* ── Refusals ─────────────────────────────────────────────────────── */

void test_HTTP_08_body_over_the_limit_is_413_and_not_read(void) {
    char req[512];
    int n = snprintf(req, sizeof(req), "POST /echo HTTP/1.1\r\nContent-Length: 300\r\n\r\n");
    memset(req + n, 'x', 300);
    uint16_t all = 0xFFFF;
    run(req, (uint32_t)n + 300, &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(413, status_of(out));
    TEST_ASSERT_TRUE_MESSAGE(consumed_total < (uint32_t)n + 300, "a refused body is not read");
}

void test_HTTP_09_malformed_heads(void) {
    static const struct {
        const char *req;
        int status;
    } cases[] = {
        {"PUT /hello HTTP/1.1\r\n\r\n", 405},
        {"GET /hello\r\n\r\n", 400},
        {"GET /hello HTTP/2.0\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", 400},
        {"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", 411},
        {"GET /nothing HTTP/1.1\r\n\r\n", 404},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_whole(cases[i].req);
        out[out_len] = '\0';
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].status, status_of(out), cases[i].req);
        uint32_t bl;
        body_of(out, out_len, &bl);
    }
    run_whole("PUT /hello HTTP/1.1\r\n\r\n");
    out[out_len] = '\0';
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "Allow: GET, HEAD, POST\r\n"), "a 405 names what is allowed");
}

void test_HTTP_10_oversized_lines_and_head(void) {
    char req[HTTP_HEAD_MAX + 256];
    int n = snprintf(req, sizeof(req), "GET /");
    memset(req + n, 'p', 300);
    n += 300;
    n += snprintf(req + n, sizeof(req) - (size_t)n, " HTTP/1.1\r\n\r\n");
    uint16_t all = 0xFFFF;
    run(req, (uint32_t)n, &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(414, status_of(out));

    n = snprintf(req, sizeof(req), "GET /");
    memset(req + n, 'p', 80);
    n += 80;
    n += snprintf(req + n, sizeof(req) - (size_t)n, " HTTP/1.1\r\n\r\n");
    run(req, (uint32_t)n, &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL_MESSAGE(414, status_of(out), "a path longer than the route table holds");

    /* A header line past HTTP_LINE_MAX is skipped, not fatal. */
    n = snprintf(req, sizeof(req), "GET /hello HTTP/1.1\r\nCookie: ");
    memset(req + n, 'c', 1000);
    n += 1000;
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n\r\n");
    run(req, (uint32_t)n, &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));

    n = snprintf(req, sizeof(req), "GET /hello HTTP/1.1\r\n");
    while (n < HTTP_HEAD_MAX + 100) {
        n += snprintf(req + n, sizeof(req) - (size_t)n, "X-Pad: 0123456789012345678901234567890123456789\r\n");
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
    run(req, (uint32_t)n, &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(431, status_of(out));
}

void test_HTTP_11_peer_closes_early(void) {
    uint16_t all = 0xFFFF;
    run("", 0, &all, 1, 0xFFFF, true);
    TEST_ASSERT_TRUE_MESSAGE(http_conn_done(&conn), "a connection that asked nothing is done");
    TEST_ASSERT_EQUAL_UINT32(0, out_len);

    const char *partial = "GET /hel";
    run(partial, (uint32_t)strlen(partial), &all, 1, 0xFFFF, true);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(400, status_of(out));

    const char *short_body = "POST /echo HTTP/1.1\r\nContent-Length: 50\r\n\r\nonly this";
    run(short_body, (uint32_t)strlen(short_body), &all, 1, 0xFFFF, true);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(400, status_of(out));
}

/* ── Streaming ────────────────────────────────────────────────────── */

/* A body five times the rx ring, into a sink that takes 100 bytes at a time
 * and is not ready for its first 50 offers -- the flash window, shut. Every
 * byte arrives once and in order. */
void test_HTTP_12_streamed_body_larger_than_the_ring(void) {
    static char req[12000];
    int n = snprintf(req, sizeof(req), "POST /upload HTTP/1.1\r\nContent-Length: 10000\r\n\r\n");
    for (int i = 0; i < 10000; i++) {
        req[n + i] = (char)(i * 7 + i / 256);
    }
    shut_passes = 50;
    uint16_t pieces[4] = {1460, 536, 1, 1460};
    run(req, (uint32_t)n + 10000, pieces, 4, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(201, status_of(out));
    TEST_ASSERT_EQUAL_UINT32(10000, sink_len);
    TEST_ASSERT_EQUAL_MEMORY(req + n, sink, 10000);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n + 10000, consumed_total);
}

void test_HTTP_13_streamed_response_through_a_small_window(void) {
    stream_len = 5000;
    uint16_t all = 0xFFFF;
    const char *req = "GET /file HTTP/1.1\r\n\r\n";
    run(req, (uint32_t)strlen(req), &all, 1, 7, false);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(200, status_of(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "X-Extra: 1\r\n"));
    uint32_t bl;
    const char *b = body_of(out, out_len, &bl);
    TEST_ASSERT_EQUAL_UINT32(5000, bl);
    for (uint32_t i = 0; i < bl; i++) {
        TEST_ASSERT_EQUAL_CHAR('a' + i % 26, b[i]);
    }
}

/* A response bigger than tx is held in work and sent as tx drains, so the
 * handler's own buffer need not outlive the call. */
void test_HTTP_14_response_larger_than_tx(void) {
    uint16_t all = 0xFFFF;
    const char *req = "GET /big HTTP/1.1\r\n\r\n";
    run(req, (uint32_t)strlen(req), &all, 1, 3, false);
    out[out_len] = '\0';
    uint32_t bl;
    const char *b = body_of(out, out_len, &bl);
    TEST_ASSERT_EQUAL_UINT32(sizeof(big), bl);
    TEST_ASSERT_EQUAL_MEMORY(big, b, sizeof(big));
}

/* "Not now" from on_complete is asked again, as the flash window requires. */
void test_HTTP_15_complete_can_wait(void) {
    complete_refusals = 30;
    run_whole("POST /slow HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc");
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(201, status_of(out));
    TEST_ASSERT_EQUAL(0, complete_refusals);
}

/* curl sends Expect: 100-continue and waits a second for it before sending
 * a large body. Say it when the body is wanted, and not when it is refused. */
void test_HTTP_16_expect_continue(void) {
    const char *req = "POST /upload HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
    uint16_t all = 0xFFFF;
    http_conn_init(&conn);
    out_len = 0;
    net_ring_write(&conn.rx, req, (uint16_t)strlen(req));
    pass(0xFFFF);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 100 Continue\r\n\r\n", out);
    net_ring_write(&conn.rx, "abcd", 4);
    pass(0xFFFF);
    out[out_len] = '\0';
    TEST_ASSERT_EQUAL(201, status_of(out));

    const char *big_req = "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 5000\r\n\r\n";
    run(big_req, (uint32_t)strlen(big_req), &all, 1, 0xFFFF, false);
    out[out_len] = '\0';
    TEST_ASSERT_NULL(strstr(out, "100 Continue"));
    TEST_ASSERT_EQUAL(413, status_of(out));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_HTTP_01_whole_request);
    RUN_TEST(test_HTTP_02_split_at_every_byte_position);
    RUN_TEST(test_HTTP_03_byte_by_byte_and_random_pieces);
    RUN_TEST(test_HTTP_04_long_header_block_and_any_case_length);
    RUN_TEST(test_HTTP_05_leading_empty_lines_are_ignored);
    RUN_TEST(test_HTTP_06_head_gets_headers_and_no_body);
    RUN_TEST(test_HTTP_07_second_request_in_the_same_read_is_not_answered);
    RUN_TEST(test_HTTP_08_body_over_the_limit_is_413_and_not_read);
    RUN_TEST(test_HTTP_09_malformed_heads);
    RUN_TEST(test_HTTP_10_oversized_lines_and_head);
    RUN_TEST(test_HTTP_11_peer_closes_early);
    RUN_TEST(test_HTTP_12_streamed_body_larger_than_the_ring);
    RUN_TEST(test_HTTP_13_streamed_response_through_a_small_window);
    RUN_TEST(test_HTTP_14_response_larger_than_tx);
    RUN_TEST(test_HTTP_15_complete_can_wait);
    RUN_TEST(test_HTTP_16_expect_continue);
    return UNITY_END();
}
