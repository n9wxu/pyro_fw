/*
 * The beep vocabulary. See beep_codes.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "beep_codes.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X_ROW(name, key, d1, d2, desc) {key, desc, BEEP_CODE(d1, d2)},
static const struct {
    const char *key;
    const char *desc;
    uint8_t def;
} rows[BEEP_REASON_COUNT] = {BEEP_REASONS(X_ROW)};
#undef X_ROW

void beep_codes_defaults(beep_table_t *t) {
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        t->code[i] = rows[i].def;
    }
}

uint8_t beep_codes_default(beep_reason_t r) {
    return (r >= 0 && r < BEEP_REASON_COUNT) ? rows[r].def : rows[BR_SYSTEM_FAILURE].def;
}

const char *beep_codes_key(beep_reason_t r) {
    return (r >= 0 && r < BEEP_REASON_COUNT) ? rows[r].key : "";
}

const char *beep_codes_description(beep_reason_t r) {
    return (r >= 0 && r < BEEP_REASON_COUNT) ? rows[r].desc : "";
}

uint8_t beep_codes_get(const beep_table_t *t, beep_reason_t r) {
    if (r < 0 || r >= BEEP_REASON_COUNT) {
        /* A caller with a bad reason still has something to report. */
        /* A caller with a bad reason still has something to say, and the
         * safe thing to say is "leave the pad". */
        return rows[BR_SYSTEM_FAILURE].def;
    }
    /* Zero is never a valid code -- validation requires both digits in 1..9
     * -- so it unambiguously means "not set yet". Falling back to the shipped
     * code keeps beep_for() correct before beep_store_load() has run, which
     * matters because a board beeps its self-test result early and because
     * the host tests build a context by hand. */
    if (!t || t->code[r] == 0) {
        return rows[r].def;
    }
    return t->code[r];
}

const char *beep_codes_strerror(beep_err_t e) {
    switch (e) {
    case BEEP_OK:
        return "ok";
    case BEEP_ERR_DIGIT_RANGE:
        return "each digit must be 1 to 9: a 0 cannot be heard";
    case BEEP_ERR_DUPLICATE:
        return "two reasons share a code and would be indistinguishable";
    default:
        return "unknown";
    }
}

beep_verdict_t beep_codes_validate(const beep_table_t *t) {
    beep_verdict_t v = {BEEP_OK, -1, "ok"};

    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        uint8_t d1 = BEEP_DIGIT1(t->code[i]);
        uint8_t d2 = BEEP_DIGIT2(t->code[i]);
        if (d1 < BEEP_DIGIT_MIN || d1 > BEEP_DIGIT_MAX || d2 < BEEP_DIGIT_MIN || d2 > BEEP_DIGIT_MAX) {
            v.err = BEEP_ERR_DIGIT_RANGE;
            v.reason = i;
            v.what = beep_codes_strerror(v.err);
            return v;
        }
    }

    /* Two reasons on one code is the failure that matters here: the operator
     * hears 2-1, looks it up, and gets the wrong answer half the time. */
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        for (int j = i + 1; j < BEEP_REASON_COUNT; j++) {
            if (t->code[i] == t->code[j]) {
                v.err = BEEP_ERR_DUPLICATE;
                v.reason = j;
                v.what = beep_codes_strerror(v.err);
                return v;
            }
        }
    }
    return v;
}

/* ── File format ──────────────────────────────────────────────────
 *
 * [beeps]
 * all_good=11
 *
 * The value is the two digits as written, not the packed byte, because the
 * file is what an operator edits and 11 is what they hear. */

static bool parse_code(const char *s, uint8_t *out) {
    if (!s || !s[0] || s[1] == '\0' || s[2] != '\0') {
        return false; /* exactly two digits */
    }
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    *out = BEEP_CODE(s[0] - '0', s[1] - '0');
    return true;
}

/* One "key=dd" line. Split out because the caller was over the complexity
 * limit the project enforces, and because this is the part with a rule in it:
 * an unrecognised key and an unparsable value both leave the table alone, so
 * a garbled line cannot silently zero a code. Whole-table validation is what
 * refuses a bad map. */
static void apply_line(char *line, beep_table_t *t) {
    if (!*line || *line == '[' || *line == ';' || *line == '#') {
        return;
    }
    char *eq = strchr(line, '=');
    if (!eq) {
        return;
    }
    *eq = '\0';
    uint8_t code;
    if (parse_code(eq + 1, &code)) {
        for (int i = 0; i < BEEP_REASON_COUNT; i++) {
            if (strcmp(line, rows[i].key) == 0) {
                t->code[i] = code;
                break;
            }
        }
    }
    *eq = '=';
}

void beep_codes_parse_ini(char *buf, beep_table_t *t) {
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }

        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }

        apply_line(line, t);

        if (!nl) {
            break;
        }
        *nl = '\n';
        line = nl + 1;
    }
}

int beep_codes_serialize_ini(const beep_table_t *t, char *buf, int max_len) {
    int pos = 0;

/* Same overflow rule as config.c: snprintf reports what it WOULD have
 * written, so an unguarded pos += n runs past the buffer. */
#define APPEND(fmt, ...)                                                                                               \
    do {                                                                                                               \
        if (pos >= max_len)                                                                                            \
            return -1;                                                                                                 \
        int n = snprintf(buf + pos, (size_t)(max_len - pos), fmt, ##__VA_ARGS__);                                      \
        if (n < 0 || n >= max_len - pos)                                                                               \
            return -1;                                                                                                 \
        pos += n;                                                                                                      \
    } while (0)

    APPEND("[beeps]\r\n");
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        APPEND("%s=%u%u\r\n", rows[i].key, (unsigned)BEEP_DIGIT1(t->code[i]), (unsigned)BEEP_DIGIT2(t->code[i]));
    }
#undef APPEND
    return pos;
}
