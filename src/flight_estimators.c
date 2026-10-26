/*
 * Every estimator flown at once [SNS-EST-07], and the simple filter
 * [FLT-APO-09]: each one's apogee rule followed, and what each reports
 * written to the flight log.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "small_fmt.h"
#include "atmosphere.h"
#include "pressure_processing.h"

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
    int n = small_snprintf(row, sizeof(row), "%s cm=%ld cm/s=%ld explains=%d", estimator_at(i)->name, height_cm,
                           speed_cms, e->explains);
    log_row(flight_ms, row, n);
}

static void say_apogee(uint8_t i, uint32_t flight_ms) {
    char row[FLIGHT_ESTIMATOR_ROW_MAX];
    int n = small_snprintf(row, sizeof(row), "%s APOGEE", estimator_at(i)->name);
    log_row(flight_ms, row, n);
}

/* [FLT-APO-09] The simple filter beside them, from the flight's ground. */
static void follow_simple(flight_context_t *ctx, const pp_sample_t *s, uint32_t flight_ms, bool report_due) {
    simple_apogee_t *f = &ctx->simple;
    if (f->pad_pa <= 0.0f)
        simple_apogee_start(f, (float)ctx->ground_pressure);
    if (!s->suspect)
        simple_apogee_reading(f, s->raw_pa, s->timestamp_ms);
    if (simple_apogee_met(f, s->timestamp_ms) && !ctx->simple_said_apogee) {
        ctx->simple_said_apogee = true;
        log_row(flight_ms, "simple APOGEE", 13);
    }
    if (!report_due)
        return;
    char row[FLIGHT_ESTIMATOR_ROW_MAX];
    int n = small_snprintf(row, sizeof(row), "simple cm=%ld cm/s=%ld released=%d",
                           (long)(simple_apogee_height_m(f) * 100.0f), (long)(simple_apogee_speed_ms(f) * 100.0f),
                           simple_apogee_released(f));
    log_row(flight_ms, row, n);
}

void flight_follow_estimators(flight_context_t *ctx, const pp_sample_t *s, uint32_t flight_ms) {
    bool report_due = s->timestamp_ms - ctx->estimators_logged_ms >= REPORT_MS;
    if (report_due)
        ctx->estimators_logged_ms = s->timestamp_ms;
    follow_simple(ctx, s, flight_ms, report_due);
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

/* [FLT-APO-11] Which filter the declared apogee is from. */
void flight_log_apogee_by(const flight_context_t *ctx) {
    char row[FLIGHT_ESTIMATOR_ROW_MAX];
    const char *by = ctx->apogee_by_simple ? "simple" : estimator_at(pp_obeyed())->name;
    int n = small_snprintf(row, sizeof(row), "apogee by %s", by);
    log_row(ctx->last_sample - ctx->launch_time, row, n);
}
