/*
 * Ascent: the thrust report, arming, the peak and apogee
 * [FLT-ASC-01..07, FLT-APO-01..04, FLT-APO-07..10, FLT-APO-12].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "pressure_processing.h"
#include "hold.h"
#include <math.h>

#define ARM_SPEED_CMS 1000
#define ARM_HEIGHT_M 30.0f
#define APOGEE_BACKDATE_MAX_MS 1500u /* [PYR-MODE-05] */
#define STILL_SIGMAS 2.0f
#define STILL_HOLD_MS 2000u

/* Only what the estimator explained can be the peak: a port error near Mach 1
 * reads as height the rocket never had. */
static void peak_at(flight_context_t *ctx, float height_m, float pressure_pa) {
    ctx->have_peak = true;
    ctx->peak_m = height_m;
    ctx->peak_pa = pressure_pa;
}

static void track_peak(flight_context_t *ctx, const pp_sample_t *s) {
    if (!s->explains || s->suspect)
        return;
    if (!ctx->have_peak || s->height_m > ctx->peak_m)
        peak_at(ctx, s->height_m, s->pressure_pa);
}

static void report_peak(flight_context_t *ctx) {
    if (ctx->have_peak)
        ctx->max_altitude_cm = (int32_t)(ctx->peak_m * 100.0f);
    else if (ctx->altitude_cm > ctx->max_altitude_cm)
        ctx->max_altitude_cm = ctx->altitude_cm;
}

static void arm_now(flight_context_t *ctx, uint32_t sample_ms) {
    ctx->pyros_armed = true;
    ctx->armed_time = sample_ms;
}

/* [FLT-ASC-04..07] A burn, then a coast: faster than the arming
 * speed, then slower, above the arming height, on a sensor giving data. */
static bool arming_due(const flight_context_t *ctx, const pp_sample_t *s) {
    return !ctx->pyros_armed && ctx->arm_height_passed && !s->suspect && ctx->max_speed_cms >= ARM_SPEED_CMS &&
           ctx->speed_cms < ARM_SPEED_CMS;
}

typedef enum { APOGEE_NOT_YET, APOGEE_BY_MODEL, APOGEE_BY_SIMPLE } apogee_by_t;

/* Where the filtered speed crossed zero, if that is recent [PYR-MODE-05]. */
static uint32_t time_of_apogee(const pp_sample_t *s) {
    if (s->accel >= 0.0f)
        return s->timestamp_ms;
    float back_ms = s->speed / s->accel * 1000.0f;
    if (back_ms < 0.0f || back_ms > (float)APOGEE_BACKDATE_MAX_MS)
        return s->timestamp_ms;
    return s->timestamp_ms - (uint32_t)back_ms;
}

/* The simple filter's top, if that is recent [PYR-MODE-05]. */
static uint32_t time_of_simple_apogee(const flight_context_t *ctx, const pp_sample_t *s) {
    uint32_t top = simple_apogee_top_ms(&ctx->simple);
    return s->timestamp_ms - top <= APOGEE_BACKDATE_MAX_MS ? top : s->timestamp_ms;
}

/* [FLT-APO-07, FLT-APO-09, FLT-APO-10, FLT-APO-12] Once the user's lockout
 * has run, the obeyed estimator's apogee, the simple filter's standing in
 * where the model is blind to the top; with `model_decides` false, the simple
 * filter's alone [DD-109, DD-113]. */
static apogee_by_t apogee_seen(flight_context_t *ctx, const pp_sample_t *s) {
    if (!ctx->pyros_armed || ctx->apogee_declared)
        return APOGEE_NOT_YET;
    if (s->timestamp_ms - ctx->launch_time < (uint32_t)ctx->config.apogee_lockout * 1000u)
        return APOGEE_NOT_YET;
    bool simple = simple_apogee_met(&ctx->simple, s->timestamp_ms);
    if (!ctx->config.model_decides)
        return simple ? APOGEE_BY_SIMPLE : APOGEE_NOT_YET;
    if (ctx->estimator_apogee[pp_obeyed()])
        return APOGEE_BY_MODEL;
    bool falling = simple_apogee_speed_ms(&ctx->simple) < 0.0f;
    bool still = s->explains && fabsf(s->speed) <= STILL_SIGMAS * s->speed_sigma;
    bool still_held = held_for(still, &ctx->model_still_since, s->timestamp_ms, STILL_HOLD_MS);
    bool model_blind = s->explains ? still_held : falling;
    return simple && simple_apogee_released(&ctx->simple) && model_blind ? APOGEE_BY_SIMPLE : APOGEE_NOT_YET;
}

/* [FLT-APO-08] The peak is a lower bound when the climb to it was not seen. */
static bool peak_is_a_lower_bound(flight_context_t *ctx, const pp_sample_t *s) {
    bool unseen = !ctx->apogee_detector[pp_obeyed()].climb_seen || !ctx->have_peak;
    if (!ctx->have_peak)
        peak_at(ctx, s->height_m, s->pressure_pa);
    return unseen;
}

state_event_t flight_detect_ascent(flight_context_t *ctx, uint32_t now) {
    pp_sample_t s;
    if (!pp_read(&s)) {
        flight_note_no_sample(ctx, now);
        return SEVT_NONE;
    }
    ctx->under_thrust = s.accel > 0.0f;
    if (s.height_m > ARM_HEIGHT_M)
        ctx->arm_height_passed = true;
    flight_take_sample(ctx, &s, ASCENT);
    if (ctx->speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = ctx->speed_cms;
    flight_note_sensor(ctx, &s, now);

    track_peak(ctx, &s);
    report_peak(ctx);
    if (arming_due(ctx, &s))
        return SEVT_ARMED;
    apogee_by_t by = apogee_seen(ctx, &s);
    if (by == APOGEE_NOT_YET)
        return SEVT_NONE;
    if (by == APOGEE_BY_SIMPLE) {
        ctx->apogee_time = time_of_simple_apogee(ctx, &s);
        float top_m = simple_apogee_top_m(&ctx->simple);
        peak_at(ctx, top_m, pp_pressure_at_m(top_m));
        ctx->peak_lower_bound = false;
    } else {
        ctx->apogee_time = time_of_apogee(&s);
        ctx->peak_lower_bound = peak_is_a_lower_bound(ctx, &s);
    }
    ctx->apogee_by_simple = by == APOGEE_BY_SIMPLE;
    report_peak(ctx);
    return SEVT_APOGEE;
}

void flight_action_armed(flight_context_t *ctx, uint32_t now) {
    (void)now;
    arm_now(ctx, ctx->last_sample);
    flight_log_event(ctx, EVT_ARMED);
}

void flight_action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_declared = true;
    ctx->under_thrust = false;
    ctx->descent_start_time = now;
    flight_log_event(ctx, EVT_APOGEE);
    flight_log_apogee_by(ctx);
    flight_log_peak(ctx);
    telemetry_queue(&ctx->telemetry,
                    (telemetry_event_t){TELEM_EVENT_APOGEE, 0, ctx->max_altitude_cm, now - ctx->launch_time});
}
