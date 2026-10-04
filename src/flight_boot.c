/*
 * Start-up: the settle, the sensor and storage checks, the resume decision,
 * the first pyro health check and the ground calibration
 * [FLT-BOOT-01..08, FLT-BOOT-12..15, FLT-BROWN-02..07, GND-TEST-05].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "flight_resume.h"
#include "pressure_processing.h"

#define SETTLE_MS 2500u
#define SENSOR_BRINGUP_MS 5000u
#define CALIBRATION_MS 10000u
#define RESUME_DEADLINE_MS 4000u /* the start may not hang here */
#define RESUME_ESTIMATE_MS 600u  /* readings behind the level and the speed [FLT-BROWN-02] */

state_event_t flight_detect_boot_settle(flight_context_t *ctx, uint32_t now) {
    bool held = hal_ground_test_asserted();
    if (held && !ctx->gt_held)
        ctx->gt_held_since = now;
    ctx->gt_held = held;
    if (now - ctx->boot_timer < SETTLE_MS)
        return SEVT_NONE;
    ctx->gt_requested = held && now - ctx->gt_held_since >= GT_BOOT_HOLD_MS;
    return SEVT_TIMER;
}

static bool not_resumed(flight_context_t *ctx, not_resumed_t why) {
    ctx->resume = (uint8_t)RESUME_NOT_FLYING;
    ctx->not_resumed = (uint8_t)why;
    return true;
}

static void resume_flight(flight_context_t *ctx, const pad_record_t *record, float pressure_pa, uint32_t now) {
    pp_resume_flight(record->ground_pressure_pa);
    flight_make_plan(ctx);
    flight_read_pyro_health(ctx);
    ctx->diag |= DIAG_RESUMED;
    ctx->ground_pressure = record->ground_pressure_pa;
    ctx->launch_time = now;
    ctx->last_sample = now;
    ctx->pressure_pa = (int32_t)pressure_pa;
    ctx->altitude_cm = (int32_t)(atmos_height_above_m(pressure_pa, (float)record->ground_pressure_pa) * 100.0f);
    ctx->max_altitude_cm = ctx->altitude_cm;
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(0, ctx->pressure_pa, ctx->altitude_cm, ASCENT, 0, EVT_LAUNCH);
    hal_log_sample(0, ctx->pressure_pa, ctx->altitude_cm, ASCENT, 0, EVT_RESUMED);
}

/* False while the estimate is not yet good enough to decide on. */
static bool decide_resume(flight_context_t *ctx, uint32_t now, state_event_t *event) {
    *event = SEVT_NONE;
    pad_record_t record;
    int n = hal_fs_read_cached(PAD_RECORD_PATH, (char *)&record, (int)sizeof(record));
    if (n != (int)sizeof(record) || !pad_record_valid(&record))
        return not_resumed(ctx, NOT_RESUMED_NO_RECORD);
    if (flight_grounded_on_usb(ctx))
        return not_resumed(ctx, NOT_RESUMED_ON_USB);
    if (now - ctx->boot_timer >= RESUME_DEADLINE_MS)
        return not_resumed(ctx, NOT_RESUMED_NO_SAMPLE);

    float pressure_pa, rate;
    if (!pp_estimate_after(RESUME_ESTIMATE_MS, &pressure_pa, &rate))
        return false;
    int32_t height_cm = (int32_t)(atmos_height_above_m(pressure_pa, (float)record.ground_pressure_pa) * 100.0f);
    int32_t speed_cms = (int32_t)(-rate * atmos_scale_height_m(pressure_pa) * 100.0f);
    resume_verdict_t verdict = resume_assess(true, height_cm, speed_cms);
    ctx->resume = (uint8_t)verdict;
    if (verdict == RESUME_NOT_FLYING)
        return not_resumed(ctx, NOT_RESUMED_AT_GROUND);
    if (verdict == RESUME_STILL)
        return true;

    resume_flight(ctx, &record, pressure_pa, now);
    ctx->speed_cms = speed_cms;
    *event = verdict == RESUME_ASCENT ? SEVT_RESUME_ASCENT : SEVT_RESUME_DESCENT;
    return true;
}

state_event_t flight_detect_boot_sensor(flight_context_t *ctx, uint32_t now) {
    if (ctx->sensor_type == SENSOR_PENDING) {
        int sensor = hal_pressure_sensor();
        if (sensor < 0 && now - ctx->boot_timer < SENSOR_BRINGUP_MS)
            return SEVT_NONE;
        ctx->sensor_type = sensor < 0 ? 0u : (uint8_t)sensor;
    }
    if (ctx->sensor_type == 0) {
        hal_telemetry_send("!SENSOR FAIL - no pressure sensor answered\r\n");
        ctx->diag |= DIAG_SENSOR_FAIL;
        return SEVT_FAULT;
    }
    if (!ctx->storage_ok) {
        hal_telemetry_send("!FS FAIL - filesystem did not mount\r\n");
        ctx->diag |= DIAG_FS_FAIL;
        return SEVT_FAULT;
    }
    if (ctx->board_mismatch) {
        hal_telemetry_send("!BOARD MISMATCH - this image is for another board\r\n");
        ctx->diag |= DIAG_BOARD_MISMATCH;
        return SEVT_FAULT;
    }
    if (ctx->config_unreadable) {
        hal_telemetry_send("!CFG FAIL - config.ini could not be read\r\n");
        ctx->diag |= DIAG_CONFIG_UNREADABLE;
        return SEVT_FAULT;
    }
    state_event_t resumed;
    if (!decide_resume(ctx, now, &resumed))
        return SEVT_NONE;
    return resumed != SEVT_NONE ? resumed : SEVT_DONE;
}

/* [FLT-BROWN-06] Rejoined on the way down: apogee is behind it. A DELAY
 * counts from here, because the time of apogee is not known. */
void flight_action_resumed_descent(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_declared = true;
    ctx->apogee_time = now;
    ctx->descent_start_time = now;
    ctx->pyros_armed = true;
    ctx->armed_time = now;
    ctx->peak_lower_bound = true;
    hal_log_sample(0, ctx->pressure_pa, ctx->altitude_cm, FALLING, 0, EVT_APOGEE);
    flight_log_peak(ctx);
}

state_event_t flight_detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    flight_make_plan(ctx);
    flight_read_pyro_health(ctx);
    ctx->boot_timer = now;
    return ctx->gt_requested ? SEVT_GROUND_TEST : SEVT_DONE;
}

void flight_action_start_calibration(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    pp_start_cal();
}

state_event_t flight_detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (pp_cal_done())
        return SEVT_CAL_DONE;
    if (now - ctx->boot_timer < CALIBRATION_MS)
        return SEVT_NONE;
    hal_telemetry_send("!CAL TIMEOUT - sensor produced no samples\r\n");
    ctx->diag |= DIAG_SENSOR_FAIL;
    return SEVT_FAULT;
}

/* The pad record's dwell is time on the pad, so it starts here. */
void flight_action_on_pad(flight_context_t *ctx, uint32_t now) {
    ctx->boot_timer = now;
    ctx->last_sample = now; /* the pad's first sample is not yet late [SNS-PRES-17] */
    ctx->ground_pressure = pp_ground_pressure();
    ctx->pressure_pa = ctx->ground_pressure;
}

state_event_t flight_detect_fault(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    return SEVT_NONE;
}

void flight_action_fault(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->announcement = (uint8_t)BR_GENERAL_FAULT;
    if (!flight_grounded_on_usb(ctx))
        flight_say_fault();
}
