/*
 * SPDX-License-Identifier: MIT
 */
#include "log_plan.h"
#include "flight_events.h"
#include <string.h>

void log_plan_init(log_plan_t *p, uint8_t rate) {
    memset(p, 0, sizeof(*p));
    p->rate = rate;
}

static void keep(log_plan_t *p, const flog_sample_t *s, log_plan_emit_fn emit, void *ctx) {
    if (s->event == EVT_NONE) {
        p->last_ms = s->time_ms;
        p->emitted_any = true;
    }
    emit(ctx, s);
}

static bool base_due(const log_plan_t *p, uint32_t t) {
    return !p->emitted_any || t - p->last_ms >= LOG_PLAN_BASE_MS;
}

static bool near_event(const log_plan_t *p, uint32_t t) {
    for (uint8_t i = 0; i < p->n_events; i++) {
        uint32_t e = p->events[i];
        if ((t > e ? t - e : e - t) <= LOG_PLAN_AROUND_MS) {
            return true;
        }
    }
    return false;
}

static const flog_sample_t *oldest(const log_plan_t *p) {
    return &p->held[(p->head + LOG_PLAN_HELD - p->n) % LOG_PLAN_HELD];
}

/* The oldest held row leaves the delay line: every event that could reach
 * it from the future has arrived, unless the line overflowed. */
static void release_oldest(log_plan_t *p, log_plan_emit_fn emit, void *ctx) {
    flog_sample_t s = *oldest(p);
    p->n--;
    if (s.event != EVT_NONE || near_event(p, s.time_ms) || base_due(p, s.time_ms)) {
        keep(p, &s, emit, ctx);
    }
}

static void note_event(log_plan_t *p, uint32_t t) {
    if (p->n_events == LOG_PLAN_EVENTS_KEPT) {
        memmove(p->events, p->events + 1, sizeof(p->events[0]) * (LOG_PLAN_EVENTS_KEPT - 1));
        p->n_events--;
    }
    p->events[p->n_events++] = t;
}

void log_plan_take(log_plan_t *p, const flog_sample_t *s, log_plan_emit_fn emit, void *ctx) {
    if (p->rate == LOG_RATE_FULL) {
        keep(p, s, emit, ctx);
        return;
    }
    if (p->rate != LOG_RATE_EVENTS) {
        if (s->event != EVT_NONE || base_due(p, s->time_ms)) {
            keep(p, s, emit, ctx);
        }
        return;
    }
    if (s->event != EVT_NONE) {
        note_event(p, s->time_ms);
    }
    if (p->n == LOG_PLAN_HELD) {
        release_oldest(p, emit, ctx);
    }
    p->held[p->head] = *s;
    p->head = (uint16_t)((p->head + 1) % LOG_PLAN_HELD);
    p->n++;
    while (p->n > 0 && s->time_ms - oldest(p)->time_ms > LOG_PLAN_AROUND_MS) {
        release_oldest(p, emit, ctx);
    }
}

void log_plan_finish(log_plan_t *p, log_plan_emit_fn emit, void *ctx) {
    while (p->n > 0) {
        release_oldest(p, emit, ctx);
    }
}
