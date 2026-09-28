/*
 * Which samples the flight log keeps [FLT-LOG-07, DD-064]:
 *
 *   LOG_RATE_1HZ     a sample row a second, and every event row
 *   LOG_RATE_EVENTS  the same, and every sample within 1 s of an event
 *   LOG_RATE_FULL    every sample
 *
 * An event row is kept at its own time in every plan. LOG_RATE_EVENTS needs
 * the second before an event, which is only known to matter once the event
 * comes, so it passes every row through a delay line of LOG_PLAN_AROUND_MS
 * and decides as each leaves: the log stays in time order.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_PLAN_H
#define LOG_PLAN_H

#include "config.h"
#include "flight_log.h"
#include <stdbool.h>
#include <stdint.h>

#define LOG_PLAN_BASE_MS 1000u
#define LOG_PLAN_AROUND_MS 1000u
/* A second of the fastest sensor with room for event rows; a faster stream
 * evicts early, so its windows are shorter than a second. */
#define LOG_PLAN_HELD 128
#define LOG_PLAN_EVENTS_KEPT 8

typedef void (*log_plan_emit_fn)(void *ctx, const flog_sample_t *s);

typedef struct {
    uint8_t rate;
    bool emitted_any;
    uint32_t last_ms; /* the last sample row written */
    flog_sample_t held[LOG_PLAN_HELD];
    uint16_t head, n;
    uint32_t events[LOG_PLAN_EVENTS_KEPT]; /* the latest event times */
    uint8_t n_events;
} log_plan_t;

void log_plan_init(log_plan_t *p, uint8_t rate);

/* A sample, or an event row (event != 0), in time order. emit is called for
 * each row the log keeps, in time order. */
void log_plan_take(log_plan_t *p, const flog_sample_t *s, log_plan_emit_fn emit, void *ctx);

/* The log ends: what is still held is decided now. */
void log_plan_finish(log_plan_t *p, log_plan_emit_fn emit, void *ctx);

#endif
