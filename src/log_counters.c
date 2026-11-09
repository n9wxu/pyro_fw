/*
 * See log_counters.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_counters.h"
#include <string.h>

#define LOG_CNT_NAME(e, name, said) name,
static const char *const NAMES[LOG_COUNTER_COUNT] = {LOG_COUNTERS(LOG_CNT_NAME)};
#undef LOG_CNT_NAME
#define LOG_CNT_SAID(e, name, said) (uint32_t)(said) << e |
static const uint32_t ON_CHANGE = LOG_COUNTERS(LOG_CNT_SAID) 0u;
#undef LOG_CNT_SAID

_Static_assert(LOG_COUNTER_COUNT <= 32, "a bit a counter in due");

void log_counters_all(log_counters_t *c) {
    c->due = (LOG_COUNTER_COUNT == 32) ? 0xffffffffu : ((1u << LOG_COUNTER_COUNT) - 1u);
}

void log_counters_note(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT]) {
    for (int i = 0; i < LOG_COUNTER_COUNT; i++)
        if (now[i] != c->last[i] && (ON_CHANGE & (1u << i)))
            c->due |= 1u << i;
}

/* Decimal without printf: this runs in the flight loop, from RAM (DD-104),
 * and printf would take a large part of the RAM with it. */
static int put_u32(char *o, uint32_t v) {
    char tmp[10];
    int n = 0, k = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        o[k++] = tmp[--n];
    return k;
}

int log_counters_row(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT], char *out, int cap) {
    static const char HEAD[] = "CNT";
    int len = (int)sizeof(HEAD) - 1;
    if (c->due == 0 || cap <= len)
        return 0;
    memcpy(out, HEAD, (size_t)len);
    for (int i = 0; i < LOG_COUNTER_COUNT; i++) {
        if (!(c->due & (1u << i)))
            continue;
        char item[24];
        int name = (int)strlen(NAMES[i]), n = 0;
        item[n++] = ' ';
        memcpy(item + n, NAMES[i], (size_t)name);
        n += name;
        item[n++] = '=';
        n += put_u32(item + n, now[i]);
        if (len + n >= cap)
            continue; /* the next row takes it */
        memcpy(out + len, item, (size_t)n);
        len += n;
        c->last[i] = now[i];
        c->due &= ~(1u << i);
    }
    out[len] = '\0';
    return len;
}
