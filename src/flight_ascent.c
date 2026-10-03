/*
 * Ascent: the thrust report, arming, the Mach flag, the peak and apogee
 * [FLT-ASC-01..07, FLT-APO-01..04, FLT-MACH-02..07].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "mach_lockout.h"
#include "pressure_processing.h"

#define ARM_SPEED_CMS 1000
#define APOGEE_SIGMAS 3.0f           /* descending by more than the rate's own uncertainty */
#define APOGEE_RISE_SIGMAS 2.0f      /* and above the lowest pressure by more than the estimate's */
#define APOGEE_BACKDATE_MAX_MS 1500u /* [PYR-MODE-05] */
#define PEAK_LOWER_BOUND_MS 2000u    /* [FLT-MACH-07] */

static void note_event(flight_context_t *ctx, uint8_t event, const char *line) {
    flight_log_event(ctx, event);
    hal_telemetry_send(line);
}

static void track_peak(flight_context_t *ctx, const pp_sample_t *s) {
    if (ctx->mach.flagged)
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

/* [FLT-ASC-04..07, FLT-MACH-06] A burn, then a coast: faster than the arming
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

/* [FLT-APO-01] Apogee is the sensor saying the rocket has stopped going up,
 * and nothing else [DD-022]. */
static bool apogee_seen(flight_context_t *ctx, const pp_sample_t *s) {
    bool past_the_minimum = s->pressure_pa > ctx->peak_pa * (1.0f + APOGEE_RISE_SIGMAS * s->log_sigma);
    bool descending = !s->suspect && s->rate > APOGEE_SIGMAS * s->rate_sigma && past_the_minimum;
    bool allowed = ctx->pyros_armed && !ctx->apogee_declared && !ctx->mach.flagged;
    return allowed && descending;
}

static state_event_t follow_mach_lock(flight_context_t *ctx, const pp_sample_t *s) {
    switch (mach_lock_in_ascent(&ctx->mach, s)) {
    case MACH_FLAGGED:
        note_event(ctx, EVT_MACH_LOCK, "!MACH LOCK\r\n");
        return SEVT_NONE;
    case MACH_RELEASED:
        ctx->peak_pa = s->pressure_pa; /* nothing from the flagged span may be the peak */
        note_event(ctx, EVT_MACH_UNLOCK, "!MACH UNLOCK\r\n");
        return SEVT_NONE;
    case MACH_APOGEE:
        if (!ctx->pyros_armed)
            arm_now(ctx, s->timestamp_ms);
        if (ctx->peak_pa <= 0.0f)
            ctx->peak_pa = s->pressure_pa;
        ctx->peak_lower_bound = true;
        ctx->apogee_time = time_of_apogee(s);
        note_event(ctx, EVT_MACH_FALLBACK, "!MACH FALLBACK\r\n");
        return SEVT_APOGEE;
    default:
        return SEVT_NONE;
    }
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
    if (mach_above_arm_height(s.pressure_pa, (float)ctx->ground_pressure))
        ctx->arm_height_passed = true;
    flight_take_sample(ctx, &s, ASCENT);
    flight_note_sensor(ctx, &s, now);

    state_event_t lock_event = follow_mach_lock(ctx, &s);
    track_peak(ctx, &s);
    report_peak(ctx, &s);
    if (lock_event != SEVT_NONE)
        return lock_event;
    if (arming_due(ctx, &s))
        return SEVT_ARMED;
    if (!apogee_seen(ctx, &s))
        return SEVT_NONE;
    ctx->apogee_time = time_of_apogee(&s);
    ctx->peak_lower_bound =
        ctx->mach.released_once && (int32_t)(ctx->apogee_time - ctx->mach.release_ms) < (int32_t)PEAK_LOWER_BOUND_MS;
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
    telemetry_queue(&ctx->telemetry,
                    (telemetry_event_t){TELEM_EVENT_APOGEE, 0, ctx->max_altitude_cm, now - ctx->launch_time});
}
