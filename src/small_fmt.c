/*
 * snprintf for the flight path's text. See small_fmt.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "small_fmt.h"

#include <stdbool.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t n;
} fmt_out_t;

static void put_char(fmt_out_t *o, char c) {
    if ((o->n + 1u) < o->cap) {
        o->buf[o->n] = c;
    }
    o->n++;
}

static void repeat(fmt_out_t *o, char c, size_t k) {
    for (size_t left = k; left != 0u; left--) {
        put_char(o, c);
    }
}

/* The digits of u, most significant first; returns how many. */
static size_t digits(char *out, unsigned long value, bool hex, bool upper) {
    unsigned long u = value;
    char rev[11];
    size_t n = 0;
    do {
        unsigned d = hex ? (unsigned)(u & 0xfu) : (unsigned)(u % 10u);
        const char ch = (char)((d < 10u) ? ((unsigned)'0' + d) : (((unsigned)(upper ? 'A' : 'a') + d) - 10u));
        rev[n] = ch;
        n++;
        u = hex ? (u >> 4) : (u / 10u);
    } while (u != 0u);
    for (size_t i = 0; i < n; i++) {
        out[i] = rev[n - 1u - i];
    }
    return n;
}

/* A conversion's flags, width and length. */
typedef struct {
    bool zero;
    bool left;
    bool lng;
    size_t width;
} spec_t;

/* p just past the '%'; returns it at the conversion character. */
static const char *parse_spec(const char *p, spec_t *sp) {
    const char *q = p;
    *sp = (spec_t){false, false, false, 0u};
    while (true) {
        if (*q == '0') {
            sp->zero = true;
        } else if (*q == '-') {
            sp->left = true;
        } else {
            break;
        }
        q++;
    }
    while ((*q >= '0') && (*q <= '9')) {
        sp->width = (sp->width * 10u) + (size_t)(*q - '0');
        q++;
    }
    if (*q == 'l') {
        sp->lng = true;
        q++;
    }
    return q;
}

/* A converted field: its characters, with a minus sign before them. */
typedef struct {
    const char *s;
    size_t len;
    bool neg;
} field_t;

static void convert_signed(const spec_t *sp, va_list *ap, char *tmp, field_t *f) {
    long v = sp->lng ? va_arg(*ap, long) : (long)va_arg(*ap, int);
    f->neg = v < 0;
    f->len = digits(tmp, f->neg ? (0uL - (unsigned long)v) : (unsigned long)v, false, false);
}

static void convert_unsigned(char c, const spec_t *sp, va_list *ap, char *tmp, field_t *f) {
    unsigned long u = sp->lng ? va_arg(*ap, unsigned long) : (unsigned long)va_arg(*ap, unsigned);
    f->len = digits(tmp, u, c != 'u', c == 'X');
}

/* The field conversion c makes of the next argument; false for one outside
 * those handled. A string is never zero-padded. */
static bool convert(char c, spec_t *sp, va_list *ap, char *tmp, field_t *f) {
    *f = (field_t){tmp, 0u, false};
    switch (c) {
    case 'c':
        tmp[0] = (char)va_arg(*ap, int);
        f->len = 1;
        return true;
    case 's':
        f->s = va_arg(*ap, const char *);
        if (f->s == NULL) {
            f->s = "(null)";
        }
        while (f->s[f->len] != '\0') {
            f->len++;
        }
        sp->zero = false;
        return true;
    case 'd':
    case 'i':
        convert_signed(sp, ap, tmp, f);
        return true;
    case 'u':
    case 'x':
    case 'X':
        convert_unsigned(c, sp, ap, tmp, f);
        return true;
    default:
        return false;
    }
}

static void emit(fmt_out_t *o, const spec_t *sp, const field_t *f) {
    size_t body = f->len + (f->neg ? 1u : 0u);
    size_t pad = (sp->width > body) ? (sp->width - body) : 0u;
    if (!sp->left && !sp->zero) {
        repeat(o, ' ', pad);
    }
    if (f->neg) {
        put_char(o, '-');
    }
    if (!sp->left && sp->zero) {
        repeat(o, '0', pad);
    }
    for (size_t i = 0; i < f->len; i++) {
        put_char(o, f->s[i]);
    }
    if (sp->left) {
        repeat(o, ' ', pad);
    }
}

/* A conversion outside those handled is copied as written. */
int small_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    fmt_out_t o = {buf, cap, 0};
    va_list args;
    va_copy(args, ap);
    const char *p = fmt;
    while (*p != '\0') {
        if (*p != '%') {
            put_char(&o, *p);
            p++;
            continue;
        }
        const char *start = p;
        spec_t sp;
        field_t f;
        char tmp[11];
        p = parse_spec(&p[1], &sp);
        if (*p == '%') {
            put_char(&o, '%');
        } else if (convert(*p, &sp, &args, tmp, &f)) {
            emit(&o, &sp, &f);
        } else {
            for (const char *q = start; (q <= p) && (*q != '\0'); q++) {
                put_char(&o, *q);
            }
            if (*p == '\0') {
                p--;
            }
        }
        p++;
    }
    va_end(args);
    if (cap != 0u) {
        buf[(o.n < cap) ? o.n : (cap - 1u)] = '\0';
    }
    return (int)o.n;
}

int small_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = small_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
