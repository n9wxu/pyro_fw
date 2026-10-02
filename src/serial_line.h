/*
 * Lines from a byte stream, for hal_serial_readline() [GND-TEST-01].
 *
 * A line ends at CR or LF; an empty one is skipped. A line that outgrows the
 * buffer is discarded whole, up to its end, so the next line is read from its
 * start rather than from the middle of the one that did not fit -- and a
 * stream with no line end in it cannot wedge the reader.
 *
 * Header-only, so the host tests link it without a build change.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SERIAL_LINE_H
#define SERIAL_LINE_H

#include <stdbool.h>
#include <string.h>

#define SERIAL_LINE_MAX 64

typedef struct {
    char buf[SERIAL_LINE_MAX];
    int len;
    bool discarding; /* in a line that did not fit */
} serial_line_t;

/* One byte in. True when it ended a line, which is then in out,
 * NUL-terminated and cut to max_len - 1. A max_len below 1 takes nothing. */
static inline bool serial_line_feed(serial_line_t *r, char c, char *out, int max_len) {
    if (c == '\n' || c == '\r') {
        bool had = r->len > 0 && !r->discarding;
        int n = r->len;
        r->len = 0;
        r->discarding = false;
        if (!had || max_len < 1)
            return false;
        if (n > max_len - 1)
            n = max_len - 1;
        memcpy(out, r->buf, (size_t)n);
        out[n] = '\0';
        return true;
    }
    if (r->discarding)
        return false;
    if (r->len >= SERIAL_LINE_MAX - 1) {
        r->len = 0;
        r->discarding = true;
        return false;
    }
    r->buf[r->len++] = c;
    return false;
}

#endif
