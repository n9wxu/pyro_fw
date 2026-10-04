/*
 * See replay.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/config.h"
#include "../src/flight_states.h"
#include "../src/hal.h"
#include "../src/pressure_processing.h"

/* Any time will do for T+0; far from zero, so no stamp is ever 0. */
#define REPLAY_T0_MS 1000000u

enum { C_TIME, C_PRESSURE, C_ALTITUDE, C_STATE, C_THRUST, C_RAW, C_TEMP, C_EVENT, C_COUNT };
static const char *const COLUMNS[C_COUNT] = {"time_ms", "pressure_pa", "altitude_cm", "state",
                                             "thrust",  "raw_pa",      "temp_c",      "event"};

typedef struct {
    int col[C_COUNT]; /* each named column's position, or -1 */
    int32_t ground_pa;
    config_t cfg;
    bool thinned; /* a row a second: the readings between are gone [DAT-08] */
} header_t;

/* Splits one line into at most max fields, in place. */
static int split(char *line, char **field, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        field[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma)
            break;
        *comma = '\0';
        p = comma + 1;
    }
    char *end = field[n - 1] + strcspn(field[n - 1], "\r\n");
    *end = '\0';
    return n;
}

/* The "# Key: value" lines, then the column names. Returns the first data
 * line, or NULL. */
static const char *read_header(const char *csv, header_t *h) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < C_COUNT; i++)
        h->col[i] = -1;
    config_set_defaults(&h->cfg);
    char ini[256] = "";
    const char *p = csv;
    while (*p == '#') {
        const char *eol = strchr(p, '\n');
        if (!eol)
            return NULL;
        char line[96];
        size_t len = (size_t)(eol - p) < sizeof(line) - 1 ? (size_t)(eol - p) : sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        line[strcspn(line, "\r")] = '\0';
        char mode[16];
        unsigned value;
        if (sscanf(line, "# Pyro1: %15s %u", mode, &value) == 2) {
            size_t k = strlen(ini);
            snprintf(ini + k, sizeof(ini) - k, "pyro1_mode=%s\npyro1_value=%u\n", mode, value);
        } else if (sscanf(line, "# Pyro2: %15s %u", mode, &value) == 2) {
            size_t k = strlen(ini);
            snprintf(ini + k, sizeof(ini) - k, "pyro2_mode=%s\npyro2_value=%u\n", mode, value);
        } else if (strncmp(line, "# Units: ", 9) == 0) {
            /* config.ini names the unit, as the header does. */
            size_t k = strlen(ini);
            snprintf(ini + k, sizeof(ini) - k, "units=%s\n", line + 9);
        } else if (strncmp(line, "# Ground Pa: ", 13) == 0) {
            h->ground_pa = (int32_t)strtol(line + 13, NULL, 10);
        } else if (strncmp(line, "# Log rate: ", 12) == 0) {
            h->thinned = strcmp(line + 12, "every sample") != 0;
        }
        p = eol + 1;
    }
    config_parse_ini(ini, &h->cfg);
    const char *eol = strchr(p, '\n');
    if (!eol)
        return NULL;
    char names[160];
    size_t len = (size_t)(eol - p) < sizeof(names) - 1 ? (size_t)(eol - p) : sizeof(names) - 1;
    memcpy(names, p, len);
    names[len] = '\0';
    char *field[16];
    int n = split(names, field, 16);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < C_COUNT; c++)
            if (strcmp(field[i], COLUMNS[c]) == 0)
                h->col[c] = i;
    return eol + 1;
}

/* Calls fn for each data row, with its fields. */
typedef void (*row_fn)(char **field, const header_t *h, void *arg);

static void each_row(const char *rows, const header_t *h, row_fn fn, void *arg) {
    const char *p = rows;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[128];
        if (len > 0 && len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = '\0';
            char *field[16];
            int n = split(line, field, 16);
            if (n > h->col[C_EVENT] && h->col[C_EVENT] >= 0)
                fn(field, h, arg);
        }
        if (!eol)
            break;
        p = eol + 1;
    }
}

static void note_logged(char **field, const header_t *h, void *arg) {
    replay_events_t *out = (replay_events_t *)arg;
    const char *ev = field[h->col[C_EVENT]];
    uint32_t t = (uint32_t)strtoul(field[h->col[C_TIME]], NULL, 10);
    if (ev[0] == '\0') {
        out->rows++;
        return;
    }
    if (strcmp(ev, "APOGEE") == 0 && out->apogee_ms == 0)
        out->apogee_ms = t;
    else if (strcmp(ev, "PYRO1") == 0 && out->pyro1_ms == 0)
        out->pyro1_ms = t;
    else if (strcmp(ev, "PYRO2") == 0 && out->pyro2_ms == 0)
        out->pyro2_ms = t;
    else if (strcmp(ev, "LANDING") == 0 && out->landing_ms == 0)
        out->landing_ms = t;
}

bool replay_logged_events(const char *csv, replay_events_t *out) {
    memset(out, 0, sizeof(*out));
    header_t h;
    const char *rows = read_header(csv, &h);
    if (!rows || h.col[C_TIME] < 0 || h.col[C_EVENT] < 0)
        return false;
    each_row(rows, &h, note_logged, out);
    return true;
}

void (*replay_row_hook)(uint32_t now_ms);

/* ── The firmware, flown on the log's readings ─────────────────────── */

#define SAMPLE_MS 20u
#define PAD_MS 9000u /* start-up, calibration, and a full ground window */

typedef struct {
    flight_context_t *ctx;
    replay_events_t *out;
    uint32_t now;     /* the loop clock */
    uint32_t base;    /* loop time of the log's T+0, once the first row is fed */
    bool flying;      /* the log's rows have begun */
    int logged_state; /* of the newest row fed */
} run_t;

static void note_decided(run_t *r) {
    const flight_context_t *ctx = r->ctx;
    uint32_t t = r->now - r->base;
    if (ctx->apogee_declared && r->out->apogee_ms == 0)
        r->out->apogee_ms = t;
    if (ctx->fire.channel[0].fired && r->out->pyro1_ms == 0)
        r->out->pyro1_ms = t;
    if (ctx->fire.channel[1].fired && r->out->pyro2_ms == 0)
        r->out->pyro2_ms = t;
    if (ctx->current_state == LANDED && r->out->landing_ms == 0)
        r->out->landing_ms = t;
}

/* One pass of the main loop on one reading. */
static void step(run_t *r, int32_t raw_pa) {
    flight_context_t *ctx = r->ctx;
    if (replay_row_hook)
        replay_row_hook(r->now);
    pp_feed(raw_pa, r->now);
    ctx->current_state = dispatch_state(ctx, r->now);
    flight_update_outputs(ctx, r->now);
    if (!r->flying)
        return;
    r->out->rows++;
    if (r->out->diverged_ms == 0 && (int)ctx->current_state != r->logged_state && ctx->current_state != PAD_IDLE)
        r->out->diverged_ms = r->now - r->base;
    note_decided(r);
}

/* The log begins at launch, so the board is first stood on a pad at the
 * log's ground pressure: started, calibrated and left to fill its ground
 * reference. A pascal of dither keeps the readings from being one value,
 * which is a stuck sensor [SNS-PRES-10]. */
static void stand_on_the_pad(run_t *r, int32_t ground_pa) {
    static const int8_t dither[] = {0, 1, 0, -1};
    for (uint32_t i = 0; i * SAMPLE_MS < PAD_MS; i++) {
        r->now = i * SAMPLE_MS;
        step(r, ground_pa + dither[i % 4u]);
    }
}

/* A log's first row is written when the launch is declared, some way into
 * the climb, and its time says how long ago the rocket left the pad. Those
 * readings are not in the log, so a steady acceleration from the pad to the
 * first row stands in for them: a step from the pad to the first row would
 * read as a rocket far past the Mach flag's speed. */
static void climb_to_the_first_row(run_t *r, int32_t ground_pa, int32_t first_raw_pa, uint32_t row_ms) {
    r->base = r->now + SAMPLE_MS;
    for (uint32_t ms = SAMPLE_MS; ms < row_ms; ms += SAMPLE_MS) {
        float part = (float)ms / (float)row_ms;
        r->now = r->base + ms;
        step(r, ground_pa + (int32_t)((float)(first_raw_pa - ground_pa) * part * part));
    }
}

static void feed_row(char **field, const header_t *h, void *arg) {
    run_t *r = (run_t *)arg;
    if (field[h->col[C_EVENT]][0] != '\0' || field[h->col[C_RAW]][0] == '\0')
        return;
    uint32_t row_ms = (uint32_t)strtoul(field[h->col[C_TIME]], NULL, 10);
    int32_t raw_pa = (int32_t)strtol(field[h->col[C_RAW]], NULL, 10);
    if (!r->flying) {
        climb_to_the_first_row(r, h->ground_pa, raw_pa, row_ms);
        r->flying = true;
    }
    r->now = r->base + row_ms;
    r->logged_state = (int)strtol(field[h->col[C_STATE]], NULL, 10);
    step(r, raw_pa);
}

bool replay_run(const char *csv, replay_events_t *out) {
    memset(out, 0, sizeof(*out));
    header_t h;
    const char *rows = read_header(csv, &h);
    if (!rows || h.thinned || h.col[C_TIME] < 0 || h.col[C_EVENT] < 0 || h.col[C_RAW] < 0 || h.col[C_STATE] < 0)
        return false;
    static flight_context_t ctx;
    if (replay_row_hook)
        replay_row_hook(0);
    (void)hal_config_save(&h.cfg);
    flight_init(&ctx);
    run_t r = {&ctx, out, 0, 0, false, 0};
    stand_on_the_pad(&r, h.ground_pa);
    if (ctx.current_state != PAD_IDLE)
        return false;
    each_row(rows, &h, feed_row, &r);
    return true;
}
