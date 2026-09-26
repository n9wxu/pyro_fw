/*
 * One HTTP exchange over a byte stream.
 *
 * The request arrives in rx and the response leaves through tx, in whatever
 * pieces the transport happens to deliver and accept. Nothing here knows
 * about segments: a request split at every byte, or two requests in one
 * read, parse the same way. The transport's whole job is to move bytes
 * between its buffers and these rings, and to close once http_conn_done().
 *
 * Every response is framed by Content-Length and carries Connection: close,
 * so one request is served per connection (RFC 9112 §9.6).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <stdbool.h>
#include <stdint.h>
#include "net_ring.h"

#define HTTP_RX_RING 2048
#define HTTP_TX_RING 2048
/* The largest gathered body or rendered response, and big enough to be a
 * littlefs file cache (FLASH_SECTOR_SIZE) for a streamed file. */
#define HTTP_WORK_SIZE 5120
#define HTTP_LINE_MAX 256 /* request line, or a header line worth reading */
#define HTTP_METHOD_MAX 8
#define HTTP_PATH_MAX 64
#define HTTP_HEAD_MAX 8192 /* the whole header block */
#define HTTP_HDR_MAX 384   /* a response header block */

typedef enum { HTTP_HEAD, HTTP_BODY, HTTP_APPLY, HTTP_SEND, HTTP_DONE } http_phase_t;
typedef enum { HTTP_BODY_DISCARD, HTTP_BODY_GATHER, HTTP_BODY_STREAM } http_body_t;

typedef struct http_conn http_conn_t;

typedef struct {
    /* Sent with every response; each line ends in CRLF. */
    const char *common_headers;

    /* The head is in: method, path and content_length are set. Answer now
     * with http_respond(), or choose http_gather() or http_stream() for the
     * body. Choosing neither discards it. */
    void (*on_head)(http_conn_t *c);

    /* Streamed body: take up to len bytes. Returns how many were taken; 0
     * means "not now", and the same bytes are offered again later. */
    uint16_t (*on_body)(http_conn_t *c, const uint8_t *data, uint16_t len);

    /* The whole body is in (gathered into work and NUL-terminated). Returns
     * false for "not now", to be asked again; otherwise it must respond. */
    bool (*on_complete)(http_conn_t *c);

    /* The body of an http_respond_stream() response: produce up to max bytes.
     * Returning 0 before the promised length is a failure. */
    uint16_t (*fill)(http_conn_t *c, uint8_t *dst, uint16_t max);
} http_handlers_t;

struct http_conn {
    net_ring_t rx, tx;
    uint8_t rx_mem[HTTP_RX_RING];
    uint8_t tx_mem[HTTP_TX_RING];
    /* One of: the gathered request body, the part of a response body that did
     * not fit in tx, or a file cache. Never two at once. */
    uint8_t work[HTTP_WORK_SIZE] __attribute__((aligned(4)));

    const http_handlers_t *h;
    http_phase_t phase;

    char method[HTTP_METHOD_MAX]; /* HEAD reads as GET, with head_only set */
    char path[HTTP_PATH_MAX];
    bool head_only;
    uint32_t content_length;

    /* Head parser. */
    bool have_request_line;
    bool have_length;
    bool chunked;
    bool expect_continue;
    uint32_t head_bytes;
    char line[HTTP_LINE_MAX];
    uint16_t line_len;
    bool line_long;

    http_body_t body_mode;
    uint32_t body_left;
    uint32_t gather_max;
    uint32_t gathered;

    bool responded;
    const uint8_t *out;
    uint32_t out_left;
    bool streaming;
    uint32_t stream_left;

    /* Set by the transport: the peer has closed, and rx holds everything it
     * sent before that. */
    bool rx_eof;
    /* The promised framing cannot be kept; the transport aborts. */
    bool failed;
    /* rx bytes taken since the transport last collected them. */
    uint32_t consumed;
};

void http_conn_init(http_conn_t *c);
void http_conn_service(http_conn_t *c, const http_handlers_t *h);

/* The whole response is in tx. Anything left in rx is not needed. */
bool http_conn_done(const http_conn_t *c);

/* For the transport's flow control: what the parser has taken from rx. */
uint32_t http_conn_take_consumed(http_conn_t *c);

/* From on_head. */
void http_gather(http_conn_t *c, uint32_t max);
void http_stream(http_conn_t *c);

/* A whole response. The body is copied, into tx or into work, so it may live
 * anywhere; up to HTTP_WORK_SIZE bytes. */
void http_respond(http_conn_t *c, uint16_t status, const char *ctype, const void *body, uint32_t len);
void http_respond_str(http_conn_t *c, uint16_t status, const char *ctype, const char *body);

/* A response whose len-byte body comes from the fill handler. extra is more
 * header lines, each ending in CRLF, or NULL. */
void http_respond_stream(http_conn_t *c, uint16_t status, const char *ctype, uint32_t len, const char *extra);

const char *http_reason(uint16_t status);

#endif
