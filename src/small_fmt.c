/*
 * snprintf for the flight path's text. See small_fmt.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "small_fmt.h"

#include <stdbool.h>

typedef struct {
    char *buf;
    size_t cap, n;
} out_t;

static void put(out_t *o, char c) {
    if (o->n + 1u < o->cap)
        o->buf[o->n] = c;
    o->n++;
}

static void repeat(out_t *o, char c, size_t k) {
    while (k--)
        put(o, c);
}

/* The digits of u, most significant first; returns how many. */
static size_t digits(char *out, unsigned long u, bool hex, bool upper) {
    char rev[11];
    size_t n = 0;
    do {
        unsigned d = hex ? (unsigned)(u & 0xfu) : (unsigned)(u % 10u);
        rev[n++] = (char)(d < 10u ? '0' + d : (upper ? 'A' : 'a') + d - 10u);
        u = hex ? u >> 4 : u / 10u;
    } while (u);
    for (size_t i = 0; i < n; i++)
        out[i] = rev[n - 1u - i];
    return n;
}

int small_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    out_t o = {buf, cap, 0};
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        const char *start = p++;
        bool zero = false, left = false, lng = false, neg = false;
        for (;; p++) {
            if (*p == '0')
                zero = true;
            else if (*p == '-')
                left = true;
            else
                break;
        }
        size_t width = 0;
        while (*p >= '0' && *p <= '9')
            width = width * 10u + (size_t)(*p++ - '0');
        if (*p == 'l') {
            lng = true;
            p++;
        }
        char tmp[11];
        const char *s = tmp;
        size_t len = 0;
        switch (*p) {
        case '%':
            put(&o, '%');
            continue;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[len])
                len++;
            zero = false;
            break;
        case 'd':
        case 'i': {
            long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
            neg = v < 0;
            len = digits(tmp, neg ? 0ul - (unsigned long)v : (unsigned long)v, false, false);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long u = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned);
            len = digits(tmp, u, *p != 'u', *p == 'X');
            break;
        }
        default:
            for (const char *q = start; q <= p && *q; q++)
                put(&o, *q);
            if (!*p)
                p--;
            continue;
        }
        size_t body = len + (neg ? 1u : 0u);
        size_t pad = width > body ? width - body : 0u;
        if (!left && !zero)
            repeat(&o, ' ', pad);
        if (neg)
            put(&o, '-');
        if (!left && zero)
            repeat(&o, '0', pad);
        for (size_t i = 0; i < len; i++)
            put(&o, s[i]);
        if (left)
            repeat(&o, ' ', pad);
    }
    if (cap)
        buf[o.n < cap ? o.n : cap - 1u] = '\0';
    return (int)o.n;
}

int small_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = small_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
