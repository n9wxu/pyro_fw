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
#include "pressure_processing.h"

/* Any time will do for T+0; far from zero, so no stamp is ever 0. */
#define REPLAY_T0_MS 1000000u

enum { C_TIME, C_PRESSURE, C_ALTITUDE, C_STATE, C_THRUST, C_RAW, C_TEMP, C_EVENT, C_COUNT };
static const char *const COLUMNS[C_COUNT] = {"time_ms", "pressure_pa", "altitude_cm", "state",
                                             "thrust",  "raw_pa",      "temp_c",      "event"};

typedef struct {
    int col[C_COUNT]; /* each named column's position, or -1 */
    int32_t ground_pa;
    config_t cfg;
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

typedef struct {
    flight_context_t *ctx;
    replay_events_t *out;
    bool started;
    /* The previous row: the sample the next reading brings out of the median. */
    uint32_t prev_ms;
    int prev_state;
    int32_t prev_raw;
} run_t;

/* One pass of the loop for the sample the median has just given, checked
 * against the state the log recorded for it. */
static void note_decided(run_t *r);

static void step(run_t *r, uint32_t now) {
    flight_context_t *ctx = r->ctx;
    if (ctx->current_state == LANDED)
        return;
    flight_state_t before = ctx->current_state;
    ctx->current_state = dispatch_state(ctx, now);
    flight_update_outputs(ctx, now);
    r->out->rows++;
    if (r->out->diverged_ms == 0 && ctx->last_sample - REPLAY_T0_MS == r->prev_ms && (int)before != r->prev_state)
        r->out->diverged_ms = r->prev_ms;
    note_decided(r);
}

static void note_decided(run_t *r) {
    flight_context_t *ctx = r->ctx;
    uint32_t t = ctx->last_sample - REPLAY_T0_MS;
    if (ctx->apogee_detected && r->out->apogee_ms == 0)
        r->out->apogee_ms = t;
    if (ctx->pyro1_fired && r->out->pyro1_ms == 0)
        r->out->pyro1_ms = t;
    if (ctx->pyro2_fired && r->out->pyro2_ms == 0)
        r->out->pyro2_ms = t;
    if (ctx->current_state == LANDED && r->out->landing_ms == 0)
        r->out->landing_ms = t;
}

static void feed_row(char **field, const header_t *h, void *arg) {
    run_t *r = (run_t *)arg;
    if (field[h->col[C_EVENT]][0] != '\0' || field[h->col[C_RAW]][0] == '\0')
        return;
    flight_context_t *ctx = r->ctx;
    uint32_t now = REPLAY_T0_MS + (uint32_t)strtoul(field[h->col[C_TIME]], NULL, 10);
    int32_t raw = (int32_t)strtol(field[h->col[C_RAW]], NULL, 10);
    if (!r->started) {
        /* The filter starts where the log says it stood, at this row's time:
         * the reading goes in first, so the resume takes its time as the
         * filter's last. */
        int32_t filtered = (int32_t)strtol(field[h->col[C_PRESSURE]], NULL, 10);
        pp_feed(raw, now);
        pp_resume_flight(h->ground_pa, filtered);
        ctx->filtered_pressure = filtered;
        ctx->last_altitude = pp_pressure_to_altitude_cm(filtered, h->ground_pa);
        ctx->last_height = pp_pressure_to_height_cm(filtered, h->ground_pa);
        ctx->last_sample = now;
        r->started = true;
        r->prev_ms = now - REPLAY_T0_MS;
        r->prev_state = (int)strtol(field[h->col[C_STATE]], NULL, 10);
        r->prev_raw = raw;
        return;
    }
    if (replay_row_hook)
        replay_row_hook(now);
    pp_feed(raw, now);
    step(r, now);
    r->prev_ms = now - REPLAY_T0_MS;
    r->prev_state = (int)strtol(field[h->col[C_STATE]], NULL, 10);
    r->prev_raw = raw;
}

bool replay_run(const char *csv, replay_events_t *out) {
    memset(out, 0, sizeof(*out));
    header_t h;
    const char *rows = read_header(csv, &h);
    if (!rows || h.col[C_TIME] < 0 || h.col[C_EVENT] < 0 || h.col[C_RAW] < 0 || h.col[C_PRESSURE] < 0)
        return false;
    static flight_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = h.cfg;
    ctx.current_state = ASCENT;
    ctx.launch_time = REPLAY_T0_MS;
    ctx.ground_pressure = h.ground_pa;
    ctx.sensor_type = 1;
    ctx.fs_ok = true;
    /* The log is of a flight that had its channels: continuity is not what
     * a replay is asked. */
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    pp_init();
    run_t r = {&ctx, out, false, 0, 0, 0};
    each_row(rows, &h, feed_row, &r);
    /* The median gives each sample a reading late, so the last one needs one
     * more reading to come out: its own again, a sample later. */
    if (r.started) {
        uint32_t now = REPLAY_T0_MS + r.prev_ms + 20u;
        if (replay_row_hook)
            replay_row_hook(now);
        pp_feed(r.prev_raw, now);
        step(&r, now);
    }
    return true;
}
