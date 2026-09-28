/*
 * SPDX-License-Identifier: MIT
 */
#include "flight_log.h"
#include "config.h"
#include "flight_events.h"
#include <stdio.h>
#include <string.h>

enum { REC_HEADER = 'H', REC_SAMPLE = 'S', REC_TEXT = 'T' };

/* type, len, id[8], name[8], pyro1 mode+value, pyro2 mode+value, units,
 * ground_pa, rate, then the board name */
#define HDR_FIXED 30
#define TEXT_FIXED 7 /* type, time, tag, len */

static const char *const TAG_NAMES[] = {"LUA", "MOCK"};

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put_str8(uint8_t *p, const char *s) {
    memset(p, 0, 8);
    for (int i = 0; i < 8 && s && s[i]; i++) {
        p[i] = (uint8_t)s[i];
    }
}

int flog_put_header(uint8_t *dst, int cap, const flog_header_t *h) {
    int board = 0;
    while (h->board && h->board[board] && board < FLOG_BOARD_MAX) {
        board++;
    }
    int n = FLOG_MAGIC_LEN + HDR_FIXED + board;
    if (n > cap) {
        return 0;
    }
    memcpy(dst, FLOG_MAGIC, FLOG_MAGIC_LEN);
    uint8_t *p = dst + FLOG_MAGIC_LEN;
    p[0] = REC_HEADER;
    p[1] = (uint8_t)(HDR_FIXED - 2 + board);
    put_str8(p + 2, h->id);
    put_str8(p + 10, h->name);
    p[18] = h->pyro1_mode;
    put16(p + 19, h->pyro1_value);
    p[21] = h->pyro2_mode;
    put16(p + 22, h->pyro2_value);
    p[24] = h->units;
    put32(p + 25, (uint32_t)h->ground_pa);
    p[29] = h->rate;
    memcpy(p + HDR_FIXED, h->board, (size_t)board);
    return n;
}

int flog_put_sample(uint8_t *dst, int cap, const flog_sample_t *s) {
    if (cap < FLOG_SAMPLE_BYTES) {
        return 0;
    }
    dst[0] = REC_SAMPLE;
    put32(dst + 1, s->time_ms);
    put32(dst + 5, (uint32_t)s->pressure_pa);
    put32(dst + 9, (uint32_t)s->altitude_cm);
    put32(dst + 13, (uint32_t)s->raw_pa);
    put16(dst + 17, (uint16_t)s->temp_dc);
    dst[19] = s->state;
    dst[20] = s->thrust;
    dst[21] = s->event;
    return FLOG_SAMPLE_BYTES;
}

int flog_put_text(uint8_t *dst, int cap, uint32_t time_ms, uint8_t tag, const char *text, int len) {
    if (len > FLOG_TEXT_MAX) {
        len = FLOG_TEXT_MAX;
    }
    if (len < 0 || TEXT_FIXED + len > cap) {
        return 0;
    }
    dst[0] = REC_TEXT;
    put32(dst + 1, time_ms);
    dst[5] = tag;
    dst[6] = (uint8_t)len;
    for (int i = 0; i < len; i++) {
        /* unsigned: plain char is signed on the host and unsigned on ARM. */
        unsigned char c = (unsigned char)text[i];
        dst[TEXT_FIXED + i] = (c == ',' || c < 0x20u || c >= 0x7fu) ? ' ' : c;
    }
    return TEXT_FIXED + len;
}

/* ── CSV ──────────────────────────────────────────────────────────── */

/* All of n bytes, or false at the log's end. */
static bool take(flog_csv_t *r, uint8_t *dst, int n) {
    while (n > 0) {
        int k = r->read(r->ctx, dst, n);
        if (k <= 0) {
            return false;
        }
        dst += k;
        n -= k;
    }
    return true;
}

/* Decimal without printf: a row is rendered for every download, and for the
 * Content-Length before it. */
static char *put_u32(char *o, uint32_t v) {
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n) {
        *o++ = tmp[--n];
    }
    return o;
}

static char *put_i32(char *o, int32_t v) {
    if (v < 0) {
        *o++ = '-';
        return put_u32(o, 0u - (uint32_t)v);
    }
    return put_u32(o, (uint32_t)v);
}

static char *put_str(char *o, const char *s) {
    while (*s) {
        *o++ = *s++;
    }
    return o;
}

static const char *rate_line(uint8_t rate) {
    switch (rate) {
    case LOG_RATE_FULL:
        return "every sample";
    case LOG_RATE_EVENTS:
        return "1 row/s, every sample within 1 s of an event";
    default:
        return "1 row/s";
    }
}

static int render_header(flog_csv_t *r) {
    uint8_t h[HDR_FIXED - 2 + FLOG_BOARD_MAX];
    uint8_t lead[2];
    if (!take(r, lead, 2) || lead[0] != REC_HEADER || lead[1] < HDR_FIXED - 2 || lead[1] > sizeof(h) ||
        !take(r, h, lead[1])) {
        return 0;
    }
    char board[FLOG_BOARD_MAX + 1];
    int board_len = lead[1] - (HDR_FIXED - 2);
    memcpy(board, h + HDR_FIXED - 2, (size_t)board_len);
    board[board_len] = '\0';
    char id[9] = {0}, name[9] = {0};
    memcpy(id, h, 8);
    memcpy(name, h + 8, 8);
    uint8_t units = h[22];
    int n = snprintf(r->line, sizeof(r->line),
                     "# %s Flight Data\n# ID: %s\n# Name: %s\n# Pyro1: %s %u\n# Pyro2: %s %u\n# Units: %s\n"
                     "# Ground Pa: %ld\n# Log rate: %s\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event\n",
                     board, id, name, config_mode_name(h[16]), (unsigned)get16(h + 17), config_mode_name(h[19]),
                     (unsigned)get16(h + 20),
                     units == 2   ? "ft"
                     : units == 1 ? "m"
                                  : "cm",
                     (long)(int32_t)get32(h + 23), rate_line(h[27]));
    return n > 0 && n < (int)sizeof(r->line) ? n : 0;
}

static int render_sample(flog_csv_t *r) {
    uint8_t s[FLOG_SAMPLE_BYTES - 1];
    if (!take(r, s, sizeof(s))) {
        return 0;
    }
    int16_t temp = (int16_t)get16(s + 16);
    uint32_t t_abs = temp < 0 ? 0u - (uint32_t)(int32_t)temp : (uint32_t)temp;
    char *o = r->line;
    o = put_u32(o, get32(s));
    *o++ = ',';
    o = put_i32(o, (int32_t)get32(s + 4));
    *o++ = ',';
    o = put_i32(o, (int32_t)get32(s + 8));
    *o++ = ',';
    o = put_u32(o, s[18]);
    *o++ = ',';
    o = put_u32(o, s[19]);
    *o++ = ',';
    o = put_i32(o, (int32_t)get32(s + 12));
    *o++ = ',';
    if (temp < 0) {
        *o++ = '-';
    }
    o = put_u32(o, t_abs / 10u);
    *o++ = '.';
    *o++ = (char)('0' + t_abs % 10u);
    *o++ = ',';
    o = put_str(o, flight_event_name(s[20]));
    *o++ = '\n';
    return (int)(o - r->line);
}

static int render_text(flog_csv_t *r) {
    uint8_t t[TEXT_FIXED - 1];
    uint8_t text[FLOG_TEXT_MAX];
    if (!take(r, t, sizeof(t)) || t[5] > FLOG_TEXT_MAX || !take(r, text, t[5])) {
        return 0;
    }
    char *o = r->line;
    o = put_u32(o, get32(t));
    o = put_str(o, ",,,,,,,");
    o = put_str(o, t[4] < sizeof(TAG_NAMES) / sizeof(TAG_NAMES[0]) ? TAG_NAMES[t[4]] : "TEXT");
    *o++ = ' ';
    memcpy(o, text, t[5]);
    o += t[5];
    *o++ = '\n';
    return (int)(o - r->line);
}

/* The next record's text into line, or false at the end. */
static bool next_line(flog_csv_t *r) {
    if (r->ended) {
        return false;
    }
    int n = 0;
    if (!r->started) {
        uint8_t magic[FLOG_MAGIC_LEN];
        r->started = true;
        if (take(r, magic, FLOG_MAGIC_LEN) && memcmp(magic, FLOG_MAGIC, FLOG_MAGIC_LEN) == 0) {
            n = render_header(r);
        }
    } else {
        uint8_t type;
        if (take(r, &type, 1)) {
            n = type == REC_SAMPLE ? render_sample(r) : type == REC_TEXT ? render_text(r) : 0;
        }
    }
    if (n <= 0) {
        r->ended = true;
        return false;
    }
    r->line_len = n;
    r->line_pos = 0;
    return true;
}

void flog_csv_init(flog_csv_t *r, flog_read_fn read, void *ctx) {
    memset(r, 0, sizeof(*r));
    r->read = read;
    r->ctx = ctx;
}

int flog_csv_read(flog_csv_t *r, char *dst, int max) {
    int done = 0;
    while (done < max) {
        if (r->line_pos == r->line_len && !next_line(r)) {
            break;
        }
        int k = r->line_len - r->line_pos;
        if (k > max - done) {
            k = max - done;
        }
        if (dst) {
            memcpy(dst + done, r->line + r->line_pos, (size_t)k);
        }
        r->line_pos += k;
        done += k;
    }
    return done;
}
