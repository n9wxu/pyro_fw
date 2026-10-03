/*
 * Ascent: the thrust report, arming, the peak and apogee
 * [FLT-ASC-01..07, FLT-APO-01..08].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "pressure_processing.h"

#define ARM_SPEED_CMS 1000
#define ARM_PRESSURE_RATIO 0.9965f   /* about 30 m above the pad */
#define APOGEE_BACKDATE_MAX_MS 1500u /* [PYR-MODE-05] */

/* Only what the estimator explained can be the peak: a port error near Mach 1
 * reads as height the rocket never had. */
static void track_peak(flight_context_t *ctx, const pp_sample_t *s) {
    if (!s->explains || s->suspect)
        return;
    if (ctx->peak_pa <= 0.0f || s->pressure_pa < ctx->peak_pa)
        ctx->peak_pa = s->pressure_pa;
}

static void report_peak(flight_context_t *ctx, const pp_sample_t *s) {
    if (ctx->peak_pa > 0.0f)
        ctx->max_altitude_cm = (int32_t)(atmos_height_above_m(ctx->peak_pa, (float)ctx->ground_pressure) * 100.0f);
    else if (s->altitude_cm > ctx->max_altitude_cm)
        ctx->max_altitude_cm = s->altitude_cm;
}

static void arm_now(flight_context_t *ctx, uint32_t sample_ms) {
    ctx->pyros_armed = true;
    ctx->armed_time = sample_ms;
}

/* [FLT-ASC-04..07] A burn, then a coast: faster than the arming
 * speed, then slower, above the arming height, on a sensor giving data. */
static bool arming_due(const flight_context_t *ctx, const pp_sample_t *s) {
    return !ctx->pyros_armed && ctx->arm_height_passed && !s->suspect && ctx->max_speed_cms >= ARM_SPEED_CMS &&
           s->speed_cms < ARM_SPEED_CMS;
}

/* Where the filtered rate crossed zero, if that is recent [PYR-MODE-05]. */
static uint32_t time_of_apogee(const pp_sample_t *s) {
    if (s->curve <= 0.0f)
        return s->timestamp_ms;
    float back_ms = s->rate / s->curve * 1000.0f;
    if (back_ms < 0.0f || back_ms > (float)APOGEE_BACKDATE_MAX_MS)
        return s->timestamp_ms;
    return s->timestamp_ms - (uint32_t)back_ms;
}

/* [FLT-APO-01] Apogee is the obeyed estimator saying the rocket has gone
 * over the top, and nothing else [DD-022, DD-092]. */
static bool apogee_seen(const flight_context_t *ctx) {
    return ctx->pyros_armed && !ctx->apogee_declared && ctx->estimator_apogee[pp_obeyed()];
}

/* [FLT-APO-08] The peak is a lower bound when the climb to it was not seen. */
static bool peak_is_a_lower_bound(flight_context_t *ctx, const pp_sample_t *s) {
    bool unseen = !ctx->apogee_detector[pp_obeyed()].climb_seen || ctx->peak_pa <= 0.0f;
    if (ctx->peak_pa <= 0.0f)
        ctx->peak_pa = s->pressure_pa;
    return unseen;
}

state_event_t flight_detect_ascent(flight_context_t *ctx, uint32_t now) {
    pp_sample_t s;
    if (!pp_read(&s)) {
        flight_note_no_sample(ctx, now);
        return SEVT_NONE;
    }
    ctx->under_thrust = s.accel_cms2 > 0;
    if (s.speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = s.speed_cms;
    if (s.pressure_pa < ARM_PRESSURE_RATIO * (float)ctx->ground_pressure)
        ctx->arm_height_passed = true;
    flight_take_sample(ctx, &s, ASCENT);
    flight_note_sensor(ctx, &s, now);

    track_peak(ctx, &s);
    report_peak(ctx, &s);
    if (arming_due(ctx, &s))
        return SEVT_ARMED;
    if (!apogee_seen(ctx))
        return SEVT_NONE;
    ctx->apogee_time = time_of_apogee(&s);
    ctx->peak_lower_bound = peak_is_a_lower_bound(ctx, &s);
    report_peak(ctx, &s);
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
    flight_log_peak(ctx);
    telemetry_queue(&ctx->telemetry,
                    (telemetry_event_t){TELEM_EVENT_APOGEE, 0, ctx->max_altitude_cm, now - ctx->launch_time});
}
