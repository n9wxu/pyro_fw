/*
 * The beep vocabulary and personalities. See beep_codes.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "beep_codes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X_ROW(name, key, desc) {key, desc},
static const struct {
    const char *key;
    const char *desc;
} rows[BEEP_REASON_COUNT] = {BEEP_REASONS(X_ROW)};
#undef X_ROW

static const char *kind_names[] = {"silent", "chirp", "tone", "code"};

const char *beep_codes_kind_name(beep_kind_t k) {
    return (k >= 0 && k < (int)(sizeof(kind_names) / sizeof(kind_names[0]))) ? kind_names[k] : "silent";
}

const char *beep_codes_key(beep_reason_t r) {
    return (r >= 0 && r < BEEP_REASON_COUNT) ? rows[r].key : "";
}

const char *beep_codes_description(beep_reason_t r) {
    return (r >= 0 && r < BEEP_REASON_COUNT) ? rows[r].desc : "";
}

/* ── The shipped personality ──────────────────────────────────────
 *
 * Eggtimer Rocketry's convention, which is what a flier at a launch site
 * already has in their ear:
 *
 *   ok_to_fly       rapid chirp            Quantum 1.09G / Quark "ready"
 *   check_pyro_1    5 beeps                Quark: 5 = no Drogue continuity
 *   check_pyro_2    4 beeps                Quark: 4 = no Main continuity
 *   system_failure  2 beeps                Classic/TRS: 2 = sensor/hardware
 *
 * Channel 1 gets the DROGUE code and channel 2 the MAIN code because that is
 * how the shipped config uses them -- pyro1 fires at apogee, pyro2 at
 * altitude. An operator who swaps the roles should swap these too, which is
 * what the other two slots are for.
 *
 * Eggtimer counts its codes with a high-low tone; a single-tone buzzer cannot,
 * so these are beep counts with a gap between repeats. */
static void shipped(beep_personality_t *p, const char *name) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);

    p->spec[BR_OK_TO_FLY] = (beep_spec_t){BK_CHIRP, 0, 0};
    p->spec[BR_CHECK_PYRO_1] = (beep_spec_t){BK_CODE, 5, 0};
    p->spec[BR_CHECK_PYRO_2] = (beep_spec_t){BK_CODE, 4, 0};
    p->spec[BR_SYSTEM_FAILURE] = (beep_spec_t){BK_CODE, 2, 0};

    p->gap_ms = 5000; /* re-announce, so silence means something is wrong */
    p->repeat = 0;    /* until launch */
    p->split_pyro = true;
}

void beep_codes_defaults(beep_table_t *t) {
    memset(t, 0, sizeof(*t));
    shipped(&t->p[0], "Default");
    shipped(&t->p[1], "Custom 1");
    shipped(&t->p[2], "Custom 2");
    t->active = 0;
}

const beep_personality_t *beep_codes_active(const beep_table_t *t) {
    static beep_personality_t fallback;

    /* A personality always has a name, so an empty one means the table was
     * never loaded -- a zeroed struct would otherwise look like a valid
     * selection of a wholly silent personality, and the board would say
     * nothing at all. It matters because a board beeps its self-test early,
     * and because the host tests build a context by hand. */
    bool usable = t && t->active < BEEP_PERSONALITY_COUNT && t->p[t->active].name[0] != '\0';
    if (!usable) {
        shipped(&fallback, "Default");
        return &fallback;
    }
    return &t->p[t->active];
}

beep_spec_t beep_codes_spec(const beep_table_t *t, beep_reason_t r) {
    const beep_personality_t *p = beep_codes_active(t);
    if (r < 0 || r >= BEEP_REASON_COUNT) {
        return p->spec[BR_SYSTEM_FAILURE];
    }
    /* With the channels merged there is one "check the pyro", so channel 2
     * resolves to channel 1's sound rather than to a second one nobody
     * configured. */
    if (r == BR_CHECK_PYRO_2 && !p->split_pyro) {
        return p->spec[BR_CHECK_PYRO_1];
    }
    return p->spec[r];
}

const char *beep_codes_strerror(beep_err_t e) {
    switch (e) {
    case BEEP_OK:
        return "ok";
    case BEEP_ERR_DIGIT_RANGE:
        return "each beep count must be 1 to 9: a 0 cannot be heard";
    case BEEP_ERR_DUPLICATE:
        return "two outcomes sound the same and would be indistinguishable";
    case BEEP_ERR_NO_ACTIVE:
        return "the selected personality does not exist";
    case BEEP_ERR_ALL_SILENT:
        return "a personality that says nothing tells an operator nothing";
    default:
        return "unknown";
    }
}

static bool spec_same(const beep_spec_t *a, const beep_spec_t *b) {
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind != BK_CODE) {
        return true; /* two chirps, two tones, two silences all sound alike */
    }
    return a->d1 == b->d1 && a->d2 == b->d2;
}

/* Which outcomes a personality can actually produce. With the channels merged
 * check_pyro_2 is never played, so it must not be compared against. */
static bool reason_live(const beep_personality_t *p, int r) {
    return !(r == BR_CHECK_PYRO_2 && !p->split_pyro);
}

static beep_verdict_t check_personality(const beep_personality_t *p, int idx) {
    beep_verdict_t v = {BEEP_OK, idx, -1, "ok"};

    int audible = 0;
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        if (!reason_live(p, i)) {
            continue;
        }
        if (p->spec[i].kind == BK_CODE) {
            uint8_t d1 = p->spec[i].d1, d2 = p->spec[i].d2;
            if (d1 < BEEP_DIGIT_MIN || d1 > BEEP_DIGIT_MAX || d2 > BEEP_DIGIT_MAX) {
                v.err = BEEP_ERR_DIGIT_RANGE;
                v.reason = i;
                v.what = beep_codes_strerror(v.err);
                return v;
            }
        }
        if (p->spec[i].kind != BK_SILENT) {
            audible++;
        }
    }

    if (audible == 0) {
        v.err = BEEP_ERR_ALL_SILENT;
        v.what = beep_codes_strerror(v.err);
        return v;
    }

    /* Two outcomes on one sound is the failure that matters: the operator
     * hears it, looks it up, and gets the wrong answer half the time. Silence
     * is exempt -- an outcome deliberately muted is not a collision. */
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        if (!reason_live(p, i) || p->spec[i].kind == BK_SILENT) {
            continue;
        }
        for (int j = i + 1; j < BEEP_REASON_COUNT; j++) {
            if (!reason_live(p, j) || p->spec[j].kind == BK_SILENT) {
                continue;
            }
            if (spec_same(&p->spec[i], &p->spec[j])) {
                v.err = BEEP_ERR_DUPLICATE;
                v.reason = j;
                v.what = beep_codes_strerror(v.err);
                return v;
            }
        }
    }
    return v;
}

beep_verdict_t beep_codes_validate(const beep_table_t *t) {
    beep_verdict_t v = {BEEP_OK, -1, -1, "ok"};

    if (t->active >= BEEP_PERSONALITY_COUNT) {
        v.err = BEEP_ERR_NO_ACTIVE;
        v.what = beep_codes_strerror(v.err);
        return v;
    }
    for (int i = 0; i < BEEP_PERSONALITY_COUNT; i++) {
        v = check_personality(&t->p[i], i);
        if (v.err != BEEP_OK) {
            return v;
        }
    }
    return v;
}

/* ── File format ──────────────────────────────────────────────────
 *
 * [beeps]
 * active=0
 * p0_name=Default
 * p0_gap=5000
 * p0_repeat=0
 * p0_split=true
 * p0_ok_to_fly=chirp
 * p0_check_pyro_1=code:5
 * p0_system_failure=code:2-2
 *
 * A spec reads the way it sounds. "code:5" is five beeps; "code:2-2" is two
 * groups of two. The file is what an operator edits. */

static bool parse_spec(const char *s, beep_spec_t *out) {
    for (int k = 0; k < (int)(sizeof(kind_names) / sizeof(kind_names[0])); k++) {
        if (strcmp(s, kind_names[k]) == 0) {
            out->kind = (uint8_t)k;
            out->d1 = out->d2 = 0;
            return true;
        }
    }
    if (strncmp(s, "code:", 5) != 0) {
        return false;
    }
    const char *d = s + 5;
    if (d[0] < '0' || d[0] > '9') {
        return false;
    }
    out->kind = BK_CODE;
    out->d1 = (uint8_t)(d[0] - '0');
    out->d2 = 0;
    if (d[1] == '\0') {
        return true;
    }
    if (d[1] != '-' || d[2] < '0' || d[2] > '9' || d[3] != '\0') {
        return false;
    }
    out->d2 = (uint8_t)(d[2] - '0');
    return true;
}

static void spec_to_str(const beep_spec_t *sp, char *out, int max) {
    if (sp->kind != BK_CODE) {
        snprintf(out, (size_t)max, "%s", beep_codes_kind_name((beep_kind_t)sp->kind));
    } else if (sp->d2 == 0) {
        snprintf(out, (size_t)max, "code:%u", (unsigned)sp->d1);
    } else {
        snprintf(out, (size_t)max, "code:%u-%u", (unsigned)sp->d1, (unsigned)sp->d2);
    }
}

static bool parse_bool(const char *s) {
    return strcmp(s, "true") == 0 || strcmp(s, "1") == 0;
}

/* One "key=value" line. Unrecognised keys and unparsable values both leave
 * the table alone, so a garbled line cannot silently mute an outcome. */
static void apply_line(char *line, beep_table_t *t) {
    if (!*line || *line == '[' || *line == ';' || *line == '#') {
        return;
    }
    char *eq = strchr(line, '=');
    if (!eq) {
        return;
    }
    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;

    if (strcmp(key, "active") == 0) {
        int a = atoi(val);
        if (a >= 0 && a < BEEP_PERSONALITY_COUNT) {
            t->active = (uint8_t)a;
        }
    } else if (key[0] == 'p' && key[1] >= '0' && key[1] < ('0' + BEEP_PERSONALITY_COUNT) && key[2] == '_') {
        beep_personality_t *p = &t->p[key[1] - '0'];
        const char *field = key + 3;
        if (strcmp(field, "name") == 0) {
            snprintf(p->name, sizeof(p->name), "%s", val);
        } else if (strcmp(field, "gap") == 0) {
            p->gap_ms = (uint16_t)atoi(val);
        } else if (strcmp(field, "repeat") == 0) {
            p->repeat = (uint8_t)atoi(val);
        } else if (strcmp(field, "split") == 0) {
            p->split_pyro = parse_bool(val);
        } else {
            for (int i = 0; i < BEEP_REASON_COUNT; i++) {
                if (strcmp(field, rows[i].key) == 0) {
                    beep_spec_t sp;
                    if (parse_spec(val, &sp)) {
                        p->spec[i] = sp;
                    }
                    break;
                }
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

    APPEND("[beeps]\r\nactive=%u\r\n", (unsigned)t->active);
    for (int i = 0; i < BEEP_PERSONALITY_COUNT; i++) {
        const beep_personality_t *p = &t->p[i];
        APPEND("p%d_name=%s\r\n", i, p->name);
        APPEND("p%d_gap=%u\r\n", i, (unsigned)p->gap_ms);
        APPEND("p%d_repeat=%u\r\n", i, (unsigned)p->repeat);
        APPEND("p%d_split=%s\r\n", i, p->split_pyro ? "true" : "false");
        for (int r = 0; r < BEEP_REASON_COUNT; r++) {
            char sv[16];
            spec_to_str(&p->spec[r], sv, (int)sizeof(sv));
            APPEND("p%d_%s=%s\r\n", i, rows[r].key, sv);
        }
    }
#undef APPEND
    return pos;
}
