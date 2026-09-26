/*
 * SPDX-License-Identifier: MIT
 */
#include "http_conn.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>

_Static_assert(HTTP_HDR_MAX + 32 <= HTTP_TX_RING, "a response header and a 100 Continue must fit an empty tx ring");

void http_conn_init(http_conn_t *c) {
    memset(c, 0, offsetof(http_conn_t, rx_mem));
    memset(&c->h, 0, sizeof(*c) - offsetof(http_conn_t, h));
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
        return status >= 500 ? "Internal Server Error" : "Error";
    }
}

/* ── Response ─────────────────────────────────────────────────────── */

static void begin_response(http_conn_t *c, uint16_t status, const char *ctype, uint32_t len, const char *extra) {
    char hdr[HTTP_HDR_MAX];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %u %s\r\n%sConnection: close\r\nContent-Type: %s\r\nContent-Length: %lu\r\n%s\r\n",
                     (unsigned)status, http_reason(status), (c->h && c->h->common_headers) ? c->h->common_headers : "",
                     ctype, (unsigned long)len, extra ? extra : "");
    c->responded = true;
    c->phase = HTTP_SEND;
    if (n < 0 || n >= (int)sizeof(hdr) || net_ring_write(&c->tx, hdr, (uint16_t)n) != (uint16_t)n) {
        c->failed = true;
    }
}

void http_respond(http_conn_t *c, uint16_t status, const char *ctype, const void *body, uint32_t len) {
    if (c->responded) {
        return;
    }
    begin_response(c, status, ctype, len, NULL);
    if (c->head_only || c->failed || len == 0) {
        return;
    }
    uint16_t k = net_ring_write(&c->tx, body, len > 0xFFFFu ? 0xFFFFu : (uint16_t)len);
    const uint8_t *rest = (const uint8_t *)body + k;
    uint32_t left = len - k;
    if (left == 0) {
        return;
    }
    bool in_work = rest >= c->work && rest + left <= c->work + sizeof(c->work);
    if (!in_work) {
        if (left > sizeof(c->work)) {
            c->failed = true;
            return;
        }
        memmove(c->work, rest, left);
        rest = c->work;
    }
    c->out = rest;
    c->out_left = left;
}

void http_respond_str(http_conn_t *c, uint16_t status, const char *ctype, const char *body) {
    http_respond(c, status, ctype, body, (uint32_t)strlen(body));
}

void http_respond_stream(http_conn_t *c, uint16_t status, const char *ctype, uint32_t len, const char *extra) {
    if (c->responded) {
        return;
    }
    begin_response(c, status, ctype, len, extra);
    c->streaming = !c->head_only;
    c->stream_left = c->head_only ? 0 : len;
}

static void pump(http_conn_t *c) {
    if (!c->responded || c->failed) {
        return;
    }
    while (c->out_left > 0) {
        uint16_t k = net_ring_write(&c->tx, c->out, c->out_left > 0xFFFFu ? 0xFFFFu : (uint16_t)c->out_left);
        if (k == 0) {
            return;
        }
        c->out += k;
        c->out_left -= k;
    }
    while (c->streaming && c->stream_left > 0) {
        uint8_t *p;
        uint16_t span = net_ring_write_span(&c->tx, &p);
        if (span == 0) {
            return;
        }
        uint16_t want = c->stream_left < span ? (uint16_t)c->stream_left : span;
        uint16_t got = c->h->fill(c, p, want);
        if (got == 0) {
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
    if (status == 405) {
        /* An Allow header is required with a 405 (RFC 9110 §15.5.6). */
        begin_response(c, status, "text/plain", (uint32_t)strlen(why), "Allow: GET, HEAD, POST\r\n");
        if (!c->failed && net_ring_write(&c->tx, why, (uint16_t)strlen(why)) != strlen(why)) {
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
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (a != name[i]) {
            return false;
        }
    }
    if (line[n] != ':') {
        return false;
    }
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    *value = v;
    return true;
}

static bool contains_token(const char *v, const char *token) {
    size_t n = strlen(token);
    for (; *v; v++) {
        size_t i = 0;
        while (i < n && v[i] && (v[i] | 32) == token[i]) {
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
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || sp1 == c->line || sp2 == sp1 + 1) {
        refuse(c, 400, "malformed request line");
        return;
    }
    size_t mlen = (size_t)(sp1 - c->line);
    size_t plen = (size_t)(sp2 - sp1 - 1);
    if (strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        refuse(c, 400, "HTTP/1.x only");
        return;
    }
    if (plen >= HTTP_PATH_MAX) {
        refuse(c, 414, "path too long");
        return;
    }
    memcpy(c->path, sp1 + 1, plen);
    c->path[plen] = '\0';
    if (mlen == 3 && strncmp(c->line, "GET", 3) == 0) {
        strcpy(c->method, "GET");
    } else if (mlen == 4 && strncmp(c->line, "HEAD", 4) == 0) {
        strcpy(c->method, "GET");
        c->head_only = true;
    } else if (mlen == 4 && strncmp(c->line, "POST", 4) == 0) {
        strcpy(c->method, "POST");
    } else {
        refuse(c, 405, "method not allowed");
        return;
    }
    c->have_request_line = true;
}

static void end_of_head(http_conn_t *c) {
    if (c->chunked) {
        /* Transfer codings are not implemented; a length is required. */
        refuse(c, 411, "send Content-Length");
        return;
    }
    c->body_left = c->content_length;
    c->body_mode = HTTP_BODY_DISCARD;
    c->h->on_head(c);
    if (c->responded) {
        return;
    }
    if (c->expect_continue && c->body_left > 0 && c->body_mode != HTTP_BODY_DISCARD) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        net_ring_write(&c->tx, cont, (uint16_t)(sizeof(cont) - 1));
    }
    c->phase = HTTP_BODY;
}

static void header_line(http_conn_t *c) {
    if (c->line_len == 0 && !c->line_long) {
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
        while (*v >= '0' && *v <= '9') {
            if (n > (UINT32_MAX - 9u) / 10u) {
                refuse(c, 413, "body too large");
                return;
            }
            n = n * 10u + (uint32_t)(*v - '0');
            v++;
            digits++;
        }
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        if (digits == 0 || *v != '\0' || (c->have_length && n != c->content_length)) {
            refuse(c, 400, "bad Content-Length");
            return;
        }
        c->content_length = n;
        c->have_length = true;
    } else if (name_is(c->line, "transfer-encoding", &v)) {
        c->chunked = !contains_token(v, "identity") || contains_token(v, "chunked");
    } else if (name_is(c->line, "expect", &v)) {
        c->expect_continue = contains_token(v, "100-continue");
    }
}

static void parse_head(http_conn_t *c) {
    uint8_t b;
    while (c->phase == HTTP_HEAD && rx_take(c, &b, 1) == 1) {
        if (++c->head_bytes > HTTP_HEAD_MAX) {
            refuse(c, 431, "header block too large");
            return;
        }
        if (b != '\n') {
            if (c->line_len < HTTP_LINE_MAX - 1) {
                c->line[c->line_len++] = (char)b;
            } else {
                c->line_long = true;
            }
            continue;
        }
        if (c->line_len > 0 && c->line[c->line_len - 1] == '\r') {
            c->line_len--;
        }
        c->line[c->line_len] = '\0';
        if (!c->have_request_line) {
            /* Empty lines before the request line are ignored (RFC 9112 §2.2). */
            if (c->line_len > 0 || c->line_long) {
                request_line(c);
            }
        } else {
            header_line(c);
        }
        c->line_len = 0;
        c->line_long = false;
    }
    if (c->phase == HTTP_HEAD && c->rx_eof && net_ring_readable(&c->rx) == 0) {
        if (c->head_bytes == 0) {
            c->phase = HTTP_DONE; /* opened and closed, asked nothing */
        } else {
            refuse(c, 400, "incomplete request");
        }
    }
}

/* ── Request body ─────────────────────────────────────────────────── */

void http_gather(http_conn_t *c, uint32_t max) {
    if (max > sizeof(c->work) - 1) {
        max = sizeof(c->work) - 1;
    }
    if (c->content_length > max) {
        refuse(c, 413, "body too large");
        return;
    }
    c->body_mode = HTTP_BODY_GATHER;
    c->gather_max = max;
    c->gathered = 0;
}

void http_stream(http_conn_t *c) {
    c->body_mode = HTTP_BODY_STREAM;
}

static void take_body(http_conn_t *c) {
    while (c->body_left > 0 && !c->responded) {
        const uint8_t *p;
        uint16_t span = net_ring_read_span(&c->rx, &p);
        if (span == 0) {
            break;
        }
        uint16_t n = c->body_left < span ? (uint16_t)c->body_left : span;
        uint16_t took = n;
        if (c->body_mode == HTTP_BODY_GATHER) {
            memcpy(c->work + c->gathered, p, n);
            c->gathered += n;
        } else if (c->body_mode == HTTP_BODY_STREAM) {
            took = c->h->on_body(c, p, n);
            if (took > n) {
                took = n;
            }
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
    if (c->body_left == 0) {
        if (c->body_mode == HTTP_BODY_GATHER) {
            c->work[c->gathered] = '\0';
        }
        c->phase = HTTP_APPLY;
    } else if (c->rx_eof && net_ring_readable(&c->rx) == 0) {
        refuse(c, 400, "body shorter than Content-Length");
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
    if (c->phase == HTTP_SEND && !c->failed && c->out_left == 0 && (!c->streaming || c->stream_left == 0)) {
        c->phase = HTTP_DONE;
    }
}
