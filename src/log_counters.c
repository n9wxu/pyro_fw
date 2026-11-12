/*
 * See log_counters.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_counters.h"
#include <string.h>

#define LOG_COUNT_U ((uint32_t)LOG_COUNTER_COUNT)

_Static_assert(LOG_COUNT_U <= 32u, "a bit a counter in due");

void log_counters_all(log_counters_t *c) {
    c->due = (LOG_COUNT_U == 32u) ? 0xffffffffu : ((1u << LOG_COUNT_U) - 1u);
}

void log_counters_note(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT]) {
#define LOG_CNT_SAID(e, name, said) ((uint32_t)(said) << (uint32_t)(e)) |
    static const uint32_t ON_CHANGE = LOG_COUNTERS(LOG_CNT_SAID) 0u;
#undef LOG_CNT_SAID
    for (uint32_t i = 0; i < LOG_COUNT_U; i++) {
        if ((now[i] != c->last[i]) && ((ON_CHANGE & (1u << i)) != 0u)) {
            c->due |= 1u << i;
        }
    }
}

/* Decimal without printf: this runs in the flight loop, from RAM (DD-104),
 * and printf would take a large part of the RAM with it. */
static int counter_decimal(char *o, uint32_t v) {
    char tmp[10];
    int n = 0;
    int k = 0;
    uint32_t rest = v;
    do {
        int at = n;
        n++;
        tmp[at] = (char)('0' + (rest % 10u));
        rest /= 10u;
    } while (rest != 0u);
    while (n != 0) {
        n--;
        o[k] = tmp[n];
        k++;
    }
    return k;
}

int log_counters_row(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT], char *out, int cap) {
#define LOG_CNT_NAME(e, name, said) name,
    static const char *const NAMES[LOG_COUNTER_COUNT] = {LOG_COUNTERS(LOG_CNT_NAME)};
#undef LOG_CNT_NAME
    static const char HEAD[] = "CNT";
    int len = (int)sizeof(HEAD) - 1;
    if ((c->due == 0u) || (cap <= len)) {
        return 0;
    }
    (void)memcpy(out, HEAD, (size_t)len);
    for (uint32_t i = 0; i < LOG_COUNT_U; i++) {
        if ((c->due & (1u << i)) == 0u) {
            continue;
        }
        char item[24];
        int name = (int)strlen(NAMES[i]);
        int n = 0;
        int at = n;
        n++;
        item[at] = ' ';
        (void)memcpy(&item[n], NAMES[i], (size_t)name);
        n += name;
        at = n;
        n++;
        item[at] = '=';
        n += counter_decimal(&item[n], now[i]);
        if ((len + n) >= cap) {
            continue; /* the next row takes it */
        }
        (void)memcpy(&out[len], item, (size_t)n);
        len += n;
        c->last[i] = now[i];
        c->due &= ~(1u << i);
    }
    out[len] = '\0';
    return len;
}
