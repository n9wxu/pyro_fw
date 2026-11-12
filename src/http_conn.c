/*
 * SPDX-License-Identifier: MIT
 */
#include "http_conn.h"
#include "small_fmt.h"
#include <stddef.h>
#include <string.h>

_Static_assert((HTTP_HDR_MAX + 32) <= HTTP_TX_RING, "a response header and a 100 Continue must fit an empty tx ring");

void http_conn_init(http_conn_t *c) {
    (void)memset(c, 0, offsetof(http_conn_t, rx_mem));
    (void)memset(&c->h, 0, sizeof(*c) - (size_t)offsetof(http_conn_t, h));
    net_ring_init(&c->rx, c->rx_mem, HTTP_RX_RING);
    net_ring_init(&c->tx, c->tx_mem, HTTP_TX_RING);
}

uint32_t http_conn_take_consumed(http_conn_t *c) {
    uint32_t n = c->consumed;
    c->consumed = 0;
    return n;
}

const char *http_reason(uint16_t status) {
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return (status >= 500u) ? "Internal Server Error" : "Error";
    }
}

/* ── Response ─────────────────────────────────────────────────────── */

static void begin_response(http_conn_t *c, uint16_t status, const char *ctype, uint32_t len, const char *extra) {
    char hdr[HTTP_HDR_MAX];
    int n = small_snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %u %s\r\n%sConnection: close\r\nContent-Type: %s\r\nContent-Length: %lu\r\n%s\r\n",
                           (unsigned)status, http_reason(status),
                           ((c->h != NULL) && (c->h->common_headers != NULL)) ? c->h->common_headers : "", ctype,
                           (unsigned long)len, (extra != NULL) ? extra : "");
    c->responded = true;
    c->phase = HTTP_SEND;
    if ((n < 0) || (n >= (int)sizeof(hdr)) || (net_ring_write(&c->tx, hdr, (uint16_t)n) != (uint16_t)n)) {
        c->failed = true;
    }
}

static void respond(http_conn_t *c, uint16_t status, const char *ctype, const void *body, uint32_t len,
                    const char *extra) {
    if (c->responded) {
        return;
    }
    begin_response(c, status, ctype, len, extra);
    if (c->head_only || c->failed || (len == 0u)) {
        return;
    }
    uint16_t k = net_ring_write(&c->tx, body, (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len);
    const uint8_t *rest = &((const uint8_t *)body)[k];
    uint32_t left = len - k;
    if (left == 0u) {
        return;
    }
    bool in_work = (rest >= c->work) && (&rest[left] <= &c->work[sizeof(c->work)]);
    if (!in_work) {
        if (left > sizeof(c->work)) {
            c->failed = true;
            return;
        }
        (void)memmove(c->work, rest, left);
        rest = c->work;
    }
    c->out = rest;
    c->out_left = left;
}

void http_respond(http_conn_t *c, uint16_t status, const char *ctype, const void *body, uint32_t len) {
    respond(c, status, ctype, body, len, NULL);
}

void http_respond_str(http_conn_t *c, uint16_t status, const char *ctype, const char *body) {
    respond(c, status, ctype, body, (uint32_t)strlen(body), NULL);
}

void http_respond_str_with(http_conn_t *c, uint16_t status, const char *ctype, const char *body, const char *extra) {
    respond(c, status, ctype, body, (uint32_t)strlen(body), extra);
}

void http_respond_stream(http_conn_t *c, uint16_t status, const char *ctype, uint32_t len, const char *extra) {
    if (c->responded) {
        return;
    }
    begin_response(c, status, ctype, len, extra);
    c->streaming = !c->head_only;
    c->stream_left = c->head_only ? 0u : len;
}

static void pump(http_conn_t *c) {
    if (!c->responded || c->failed) {
        return;
    }
    while (c->out_left > 0u) {
        uint16_t k = net_ring_write(&c->tx, c->out, (c->out_left > 0xFFFFu) ? 0xFFFFu : (uint16_t)c->out_left);
        if (k == 0u) {
            return;
        }
        c->out = &c->out[k];
        c->out_left -= k;
    }
    while (c->streaming && (c->stream_left > 0u)) {
        uint8_t *p;
        uint16_t span = net_ring_write_span(&c->tx, &p);
        if (span == 0u) {
            return;
        }
        uint16_t want = (c->stream_left < span) ? (uint16_t)c->stream_left : span;
        uint16_t got = c->h->fill(c, p, want);
        if (got == 0u) {
            c->failed = true;
            return;
        }
        if (got > want) {
            got = want;
        }
        net_ring_commit(&c->tx, got);
        c->stream_left -= got;
    }
}

bool http_conn_done(const http_conn_t *c) {
    return c->phase == HTTP_DONE;
}

bool http_conn_wants_service(const http_conn_t *c) {
    if (c->failed) {
        return false;
    }
    bool input = (net_ring_readable(&c->rx) > 0u) || c->rx_eof;
    switch (c->phase) {
    case HTTP_HEAD:
        return input;
    case HTTP_BODY:
        return input || (c->body_left == 0u);
    case HTTP_APPLY:
        return true;
    case HTTP_SEND: {
        bool more = (c->out_left > 0u) || (c->streaming && (c->stream_left > 0u));
        return !more || (net_ring_writable(&c->tx) > 0u);
    }
    default:
        return false;
    }
}

/* ── Request head ─────────────────────────────────────────────────── */

static uint16_t rx_take(http_conn_t *c, void *dst, uint16_t n) {
    uint16_t k = net_ring_read(&c->rx, dst, n);
    c->consumed += k;
    return k;
}

static void rx_drop(http_conn_t *c, uint16_t n) {
    net_ring_discard(&c->rx, n);
    c->consumed += n;
}

static void refuse(http_conn_t *c, uint16_t status, const char *why) {
    if (status == 405u) {
        /* An Allow header is required with a 405 (RFC 9110 §15.5.6). */
        begin_response(c, status, "text/plain", (uint32_t)strlen(why), "Allow: GET, HEAD, POST\r\n");
        if (!c->failed && (net_ring_write(&c->tx, why, (uint16_t)strlen(why)) != strlen(why))) {
            c->failed = true;
        }
        return;
    }
    http_respond_str(c, status, "text/plain", why);
}

static bool name_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = line[i];
        if ((a >= 'A') && (a <= 'Z')) {
            a = (char)(a + 32);
        }
        if (a != name[i]) {
            return false;
        }
    }
    if (line[n] != ':') {
        return false;
    }
    const char *v = &line[n + 1u];
    while ((*v == ' ') || (*v == '\t')) {
        v++;
    }
    *value = v;
    return true;
}

static bool contains_token(const char *value, const char *token) {
    size_t n = strlen(token);
    for (const char *v = value; *v != '\0'; v++) {
        size_t i = 0;
        while ((i < n) && (v[i] != '\0') && (((int)v[i] | 32) == (int)token[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static void request_line(http_conn_t *c) {
    if (c->line_long) {
        refuse(c, 414, "request line too long");
        return;
    }
    const char *sp1 = strchr(c->line, ' ');
    const char *sp2 = (sp1 != NULL) ? strchr(&sp1[1], ' ') : NULL;
    if ((sp1 == NULL) || (sp2 == NULL) || (sp1 == c->line) || (sp2 == &sp1[1])) {
        refuse(c, 400, "malformed request line");
        return;
    }
    size_t mlen = strcspn(c->line, " ");
    size_t plen = strcspn(&sp1[1], " ");
    if (strncmp(&sp2[1], "HTTP/1.", 7) != 0) {
        refuse(c, 400, "HTTP/1.x only");
        return;
    }
    if (plen >= (size_t)HTTP_PATH_MAX) {
        refuse(c, 414, "path too long");
        return;
    }
    (void)memcpy(c->path, &sp1[1], plen);
    c->path[plen] = '\0';
    if ((mlen == 3u) && (strncmp(c->line, "GET", 3) == 0)) {
        (void)strcpy(c->method, "GET");
    } else if ((mlen == 4u) && (strncmp(c->line, "HEAD", 4) == 0)) {
        (void)strcpy(c->method, "GET");
        c->head_only = true;
    } else if ((mlen == 4u) && (strncmp(c->line, "POST", 4) == 0)) {
        (void)strcpy(c->method, "POST");
    } else {
        refuse(c, 405, "method not allowed");
        return;
    }
    c->have_request_line = true;
}

static void end_of_head(http_conn_t *c) {
    /* Transfer codings are not implemented; a POST states its length. */
    if (c->chunked || ((strcmp(c->method, "POST") == 0) && !c->have_length)) {
        refuse(c, 411, "send Content-Length");
        return;
    }
    c->body_left = c->content_length;
    c->body_mode = HTTP_BODY_DISCARD;
    c->h->on_head(c);
    if (c->responded) {
        return;
    }
    if (c->expect_continue && (c->body_left > 0u) && (c->body_mode != HTTP_BODY_DISCARD)) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        (void)net_ring_write(&c->tx, cont, (uint16_t)(sizeof(cont) - 1u));
    }
    c->phase = HTTP_BODY;
}

static void header_line(http_conn_t *c) {
    if ((c->line_len == 0u) && !c->line_long) {
        end_of_head(c);
        return;
    }
    if (c->line_long) {
        return; /* longer than any header read here */
    }
    const char *v;
    if (name_is(c->line, "content-length", &v)) {
        uint32_t n = 0;
        int digits = 0;
        while ((*v >= '0') && (*v <= '9')) {
            if (n > (UINT32_MAX - 9u) / 10u) {
                refuse(c, 413, "body too large");
                return;
            }
            n = (n * 10u) + (uint32_t)(*v - '0');
            v++;
            digits++;
        }
        while ((*v == ' ') || (*v == '\t')) {
            v++;
        }
        if ((digits == 0) || (*v != '\0') || (c->have_length && (n != c->content_length))) {
            refuse(c, 400, "bad Content-Length");
            return;
        }
        c->content_length = n;
        c->have_length = true;
    } else if (name_is(c->line, "transfer-encoding", &v)) {
        c->chunked = !contains_token(v, "identity") || contains_token(v, "chunked");
    } else if (name_is(c->line, "expect", &v)) {
        c->expect_continue = contains_token(v, "100-continue");
    } else if (name_is(c->line, "host", &v)) {
        size_t n = strlen(v);
        while ((n > 0u) && ((v[n - 1u] == ' ') || (v[n - 1u] == '\t'))) {
            n--;
        }
        /* One Host, and one that fits; otherwise one that names nothing. */
        bool usable = (c->host[0] == '\0') && (n < sizeof(c->host));
        (void)memcpy(c->host, usable ? v : "?", usable ? n : 1u);
        c->host[usable ? n : 1u] = '\0';
    } else if (name_is(c->line, "x-pyro", &v)) {
        c->x_pyro = (v[0] == '1') && ((v[1] == '\0') || (v[1] == ' ') || (v[1] == '\t'));
    } else {
        /* any other header is not read */
    }
}

static void parse_head(http_conn_t *c) {
    const uint16_t line_room = HTTP_LINE_MAX - 1;
    uint8_t b;
    while ((c->phase == HTTP_HEAD) && (rx_take(c, &b, 1) == 1u)) {
        if (++c->head_bytes > HTTP_HEAD_MAX) {
            refuse(c, 431, "header block too large");
            return;
        }
        if (b != (uint8_t)'\n') {
            if (c->line_len < line_room) {
                c->line[c->line_len] = (char)b;
                c->line_len++;
            } else {
                c->line_long = true;
            }
            continue;
        }
        if ((c->line_len > 0u) && (c->line[c->line_len - 1u] == '\r')) {
            c->line_len--;
        }
        c->line[c->line_len] = '\0';
        if (!c->have_request_line) {
            /* Empty lines before the request line are ignored (RFC 9112 §2.2). */
            if ((c->line_len > 0u) || c->line_long) {
                request_line(c);
            }
        } else {
            header_line(c);
        }
        c->line_len = 0;
        c->line_long = false;
    }
    if ((c->phase == HTTP_HEAD) && c->rx_eof && (net_ring_readable(&c->rx) == 0u)) {
        if (c->head_bytes == 0u) {
            c->phase = HTTP_DONE; /* opened and closed, asked nothing */
        } else {
            refuse(c, 400, "incomplete request");
        }
    }
}

/* ── Request body ─────────────────────────────────────────────────── */

void http_gather(http_conn_t *c, uint32_t max) {
    uint32_t limit = max;
    if (limit > (sizeof(c->work) - 1u)) {
        limit = sizeof(c->work) - 1u;
    }
    if (c->content_length > limit) {
        refuse(c, 413, "body too large");
        return;
    }
    c->body_mode = HTTP_BODY_GATHER;
    c->gather_max = limit;
    c->gathered = 0;
}

void http_stream(http_conn_t *c) {
    c->body_mode = HTTP_BODY_STREAM;
}

static void take_body(http_conn_t *c) {
    while ((c->body_left > 0u) && !c->responded) {
        const uint8_t *p;
        uint16_t span = net_ring_read_span(&c->rx, &p);
        if (span == 0u) {
            break;
        }
        uint16_t n = (c->body_left < span) ? (uint16_t)c->body_left : span;
        uint16_t took = n;
        if (c->body_mode == HTTP_BODY_GATHER) {
            (void)memcpy(&c->work[c->gathered], p, n);
            c->gathered += n;
        } else if (c->body_mode == HTTP_BODY_STREAM) {
            took = c->h->on_body(c, p, n);
            if (took > n) {
                took = n;
            }
        } else {
            /* discarded */
        }
        rx_drop(c, took);
        c->body_left -= took;
        if (took < n) {
            return; /* the sink is not ready: same bytes next time */
        }
    }
    if (c->responded) {
        return; /* answered mid-body; the rest is not needed */
    }
    if (c->body_left == 0u) {
        if (c->body_mode == HTTP_BODY_GATHER) {
            c->work[c->gathered] = '\0';
        }
        c->phase = HTTP_APPLY;
    } else if (c->rx_eof && (net_ring_readable(&c->rx) == 0u)) {
        refuse(c, 400, "body shorter than Content-Length");
    } else {
        /* more of the body is still to come */
    }
}

/* ── The exchange ─────────────────────────────────────────────────── */

void http_conn_service(http_conn_t *c, const http_handlers_t *h) {
    c->h = h;
    if (c->phase == HTTP_HEAD) {
        parse_head(c);
    }
    if (c->phase == HTTP_BODY) {
        take_body(c);
    }
    if (c->phase == HTTP_APPLY) {
        if (c->h->on_complete(c) && !c->responded) {
            http_respond_str(c, 500, "text/plain", "no response");
        }
    }
    pump(c);
    if ((c->phase == HTTP_SEND) && !c->failed && (c->out_left == 0u) && (!c->streaming || (c->stream_left == 0u))) {
        c->phase = HTTP_DONE;
    }
}

/* ── Where a request came from ────────────────────────────────────── */

static bool same_nocase(const char *a, size_t n, const char *b) {
    if (strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (((int)a[i] | 32) != ((int)b[i] | 32)) {
            return false;
        }
    }
    return true;
}

bool http_host_is_board(const char *host, uint8_t subnet) {
    size_t n = strlen(host);
    const char *colon = strchr(host, ':');
    if (colon != NULL) {
        if (strcmp(colon, ":80") != 0) {
            return false;
        }
        n = strcspn(host, ":");
    }
    if ((n > 0u) && (host[n - 1u] == '.')) {
        n--; /* the root label of a fully qualified name */
    }
    if (same_nocase(host, n, "pyro.local")) {
        return true;
    }
    /* pyro-<n>.local: the name mDNS takes after a conflict (net_glue.c). */
    static const char prefix[] = "pyro-";
    static const char suffix[] = ".local";
    const size_t np = sizeof(prefix) - 1u;
    const size_t ns = sizeof(suffix) - 1u;
    if ((n > (np + ns)) && same_nocase(host, np, prefix) && same_nocase(&host[n - ns], ns, suffix)) {
        size_t digits = n - np - ns;
        bool all_digits = digits <= 3u;
        for (size_t i = 0; (i < digits) && all_digits; i++) {
            all_digits = (host[np + i] >= '0') && (host[np + i] <= '9');
        }
        if (all_digits) {
            return true;
        }
    }
    char own[16];
    int k = small_snprintf(own, sizeof(own), "192.168.%u.1", (unsigned)subnet);
    return (k > 0) && ((size_t)k == n) && (strncmp(host, own, n) == 0);
}

/* A page on another site can make a browser send the board a request: by a
 * name it rebinds to the board's address, which the Host header still
 * carries, or by a form or a simple fetch, which cannot add a header of its
 * own without a CORS preflight, and the board grants none. So a request must
 * name the board, and a POST must carry X-Pyro: 1. A request with no Host is
 * malformed (RFC 9112 §3.2). */
uint16_t http_origin_refusal(const http_conn_t *c, uint8_t subnet) {
    if (c->host[0] == '\0') {
        return 400;
    }
    if (!http_host_is_board(c->host, subnet)) {
        return 403;
    }
    if ((strcmp(c->method, "POST") == 0) && !c->x_pyro) {
        return 403;
    }
    return 0;
}
