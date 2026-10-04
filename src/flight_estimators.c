/*
 * Every estimator flown at once [SNS-EST-07]: each one's apogee detector
 * followed, and what each reports written to the flight log.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "pressure_processing.h"
#include <stdio.h>

#define REPORT_MS 1000u

static void log_row(uint32_t flight_ms, const char *text, int len) {
    if (len > 0)
        hal_log_estimator(flight_ms, text, len);
}

static void report(const flight_context_t *ctx, const pp_sample_t *s, uint8_t i, uint32_t flight_ms) {
    const estimate_t *e = &s->by_estimator[i];
    long height_cm = (long)(atmos_height_above_m(e->pressure_pa, (float)ctx->ground_pressure) * 100.0f);
    long speed_cms = (long)(-e->rate * atmos_scale_height_m(e->pressure_pa) * 100.0f);
    char row[FLIGHT_ESTIMATOR_ROW_MAX];
    int n = snprintf(row, sizeof(row), "%s cm=%ld cm/s=%ld explains=%d", estimator_at(i)->name, height_cm, speed_cms,
                     e->explains);
    log_row(flight_ms, row, n);
}

static void say_apogee(uint8_t i, uint32_t flight_ms) {
    char row[FLIGHT_ESTIMATOR_ROW_MAX];
    int n = snprintf(row, sizeof(row), "%s APOGEE", estimator_at(i)->name);
    log_row(flight_ms, row, n);
}

void flight_follow_estimators(flight_context_t *ctx, const pp_sample_t *s, uint32_t flight_ms) {
    bool report_due = s->timestamp_ms - ctx->estimators_logged_ms >= REPORT_MS;
    if (report_due)
        ctx->estimators_logged_ms = s->timestamp_ms;
    for (uint8_t i = 0; i < estimator_count(); i++) {
        ctx->estimator_apogee[i] =
            apogee_detected(&ctx->apogee_detector[i], &s->by_estimator[i], !s->suspect, s->timestamp_ms);
        if (ctx->estimator_apogee[i] && !ctx->estimator_said_apogee[i]) {
            ctx->estimator_said_apogee[i] = true;
            say_apogee(i, flight_ms);
        }
        if (report_due)
            report(ctx, s, i, flight_ms);
    }
}
