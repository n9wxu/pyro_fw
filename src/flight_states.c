/*
 * Flight state machine: event-driven with transition table.
 * See REQUIREMENTS.md for requirement definitions.
 * See TRACEABILITY.md for requirement-to-test mapping.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "flight_states.h"
#include "pressure_processing.h"
#include "telemetry_formatter.h"
#include "ground_test.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ring buffer ──────────────────────────────────────────────────── */

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    /* v2-9: log every in-flight sample; events tagged later by buf_tag_event */
    if ((flight_state_t)st >= ASCENT) {
        hal_log_sample(time_ms, pressure, altitude, st, ctx->under_thrust, EVT_NONE);
    }
    if (ctx->buf_count == FLIGHT_BUF_SIZE) {
        ctx->buf_tail = (ctx->buf_tail + 1) % FLIGHT_BUF_SIZE;
        ctx->buf_count--;
    }
    flight_sample_t *s = &ctx->flight_buffer[ctx->buf_head];
    s->time_ms = time_ms;
    s->pressure_pa = pressure;
    s->altitude_cm = altitude;
    s->state = st;
    s->under_thrust = 0;
    s->event = EVT_NONE;
    s->event_data = 0;
    ctx->buf_head = (ctx->buf_head + 1) % FLIGHT_BUF_SIZE;
    ctx->buf_count++;
    /* csv_track_sample() removed: incremental logger retired, hal_log owns flight record [v2-9] */
}

static void buf_tag_event(flight_context_t *ctx, uint8_t event) { /* [DAT-03] */
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].event = event;
    /* v2-9: also emit a timestamped event line to the HAL log */
    if (hal_log_active()) {
        const flight_sample_t *s = &ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE];
        if ((flight_state_t)s->state >= ASCENT)
            hal_log_sample(s->time_ms, s->pressure_pa, s->altitude_cm, s->state, s->under_thrust, event);
    }
}

#define MAX_ALTITUDE_CM 800000

static int32_t cm_to_units(int32_t cm, uint8_t units) {
    switch (units) {
    case 1:
        return cm / 100;
    case 2:
        return cm * 100 / 3048;
    default:
        return cm;
    }
}

/* ── Pyro firing logic ────────────────────────────────────────────── */

/* [PYR-MODE-01..04, PYR-ALT-01] See IMPLEMENTATION.md "Altitude Limitations"
 * for behavior above 8000m. */
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected)
        return false; /* [PYR-SAFE-04] */
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    int32_t clamped = ((int32_t)value > max_units) ? max_units : (int32_t)value;
    int32_t fallen = cm_to_units(ctx->max_altitude - ctx->last_altitude, ctx->config.units);
    int32_t agl = cm_to_units(ctx->last_altitude, ctx->config.units);
    int32_t speed = cm_to_units(-ctx->vertical_speed_cms, ctx->config.units);
    uint32_t delay_s = (hal_time_ms() - ctx->apogee_time) / 1000;
    switch (mode) {
    case PYRO_MODE_FALLEN:
        return fallen >= clamped;
    case PYRO_MODE_AGL:
        return agl <= clamped;
    case PYRO_MODE_SPEED:
        return speed >= clamped;
    case PYRO_MODE_DELAY:
        return delay_s >= value;
    default:
        return false;
    }
}

/* [PYR-SAFE-01..03] */
static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
    if (!ctx->pyro1_fired && !hal_pyro_is_firing() && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
        hal_pyro_fire(1);
        ctx->pyro1_fired = true;
        ctx->pyro1_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO1_FIRE);
        telemetry_pyro_fire(1, ctx->last_altitude, now - ctx->launch_time);
    }
    if (!ctx->pyro2_fired && !hal_pyro_is_firing() && ctx->pyro2_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
        hal_pyro_fire(2);
        ctx->pyro2_fired = true;
        ctx->pyro2_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO2_FIRE);
        telemetry_pyro_fire(2, ctx->last_altitude, now - ctx->launch_time);
    }
}

/* [PYR-FAULT-02/03] Check FLAG pin after fire for overcurrent */
static void check_pyro_fault(flight_context_t *ctx) {
    if (ctx->pyro1_fired && !ctx->pyro1_fault && hal_pyro_fault(1)) {
        ctx->pyro1_fault = true;
        buf_tag_event(ctx, EVT_PYRO1_FAULT);
    }
    if (ctx->pyro2_fired && !ctx->pyro2_fault && hal_pyro_fault(2)) {
        ctx->pyro2_fault = true;
        buf_tag_event(ctx, EVT_PYRO2_FAULT);
    }
}

/* [PYR-VERIFY-01] Post-fire continuity check: if pyro didn't open, it failed */
static void check_post_fire_verify(flight_context_t *ctx, uint32_t now) {
    if (ctx->pyro1_fired && !ctx->pyro1_verify_fail && ctx->pyro1_fire_time > 0 && now - ctx->pyro1_fire_time > 500 &&
        now - ctx->pyro1_fire_time < 600) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (c1.good && !c1.open) {
            ctx->pyro1_verify_fail = true;
            buf_tag_event(ctx, EVT_PYRO1_NOPEN);
        }
    }
    if (ctx->pyro2_fired && !ctx->pyro2_verify_fail && ctx->pyro2_fire_time > 0 && now - ctx->pyro2_fire_time > 500 &&
        now - ctx->pyro2_fire_time < 600) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (c2.good && !c2.open) {
            ctx->pyro2_verify_fail = true;
            buf_tag_event(ctx, EVT_PYRO2_NOPEN);
        }
    }
}

/* [PYR-REFIRE-01] Re-attempt if still ballistic 1-1.5s after first fire */
static void check_refire(flight_context_t *ctx, uint32_t now) {
    bool ballistic = ctx->vertical_speed_cms < -3000;
    if (ctx->pyro1_fired && ctx->pyro1_fire_time > 0 && now - ctx->pyro1_fire_time > 1000 &&
        now - ctx->pyro1_fire_time < 1500) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c1.open) {
            hal_pyro_fire(1);
            ctx->pyro1_fire_time = now;
        }
    }
    if (ctx->pyro2_fired && ctx->pyro2_fire_time > 0 && now - ctx->pyro2_fire_time > 1000 &&
        now - ctx->pyro2_fire_time < 1500) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c2.open) {
            hal_pyro_fire(2);
            ctx->pyro2_fire_time = now;
        }
    }
}

/* Config parser moved to config.c — X-macro generated [CFG-TABLE-01] */

/* ── Event detectors ──────────────────────────────────────────────── */

/* [FLT-BOOT-04, FLT-BOOT-09] */
static state_event_t detect_boot_settle(flight_context_t *ctx, uint32_t now) {
    return (now - ctx->boot_timer >= 2500) ? SEVT_TIMER : SEVT_NONE;
}

/* [SYS-STATUS-02] */
static state_event_t detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    (void)now;
    hal_continuity_t c1, c2;
    hal_pyro_check(&c1, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->boot_timer = now;
    return SEVT_DONE;
}

/* [FLT-BOOT-08] Calibration is handled by pressure_processing layer.
 * pp_start_cal() was called in action_cal_init(); we just poll pp_cal_done(). */
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    return pp_cal_done() ? SEVT_CAL_DONE : SEVT_NONE;
}

static void update_continuity_and_buzzer(flight_context_t *ctx, uint32_t now) { /* [PYR-CONT-01, PYR-ALT-02] */
    if (now - ctx->last_cont_check <= 1000)
        return;
    hal_continuity_t c1, c2;
    hal_pyro_check(&c1, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->pyro1_adc = c1.raw_adc;
    ctx->pyro2_adc = c2.raw_adc;
    ctx->last_cont_check = now;

    if (ctx->buzzer_started)
        return;
    ctx->buzzer_started = true;
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    bool p1_over = (ctx->config.pyro1_mode != PYRO_MODE_DELAY && ctx->config.pyro1_value > max_units);
    bool p2_over = (ctx->config.pyro2_mode != PYRO_MODE_DELAY && ctx->config.pyro2_value > max_units);
    uint8_t code = BEEP_ALL_GOOD;
    if (p1_over || p2_over)
        code = BEEP_CFG_RANGE;
    else if (!c1.good)
        code = c1.open ? BEEP_P1_OPEN : BEEP_P1_SHORT;
    else if (!c2.good)
        code = c2.open ? BEEP_P2_OPEN : BEEP_P2_SHORT;
    ctx->last_status_code = code; /* [GND-TEST-01] remember for BEEP STATUS replay */
    buzzer_play_code(code, 2);    /* [BUZ-02] play status code twice then stop */
}

/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-RATE-01, DD-016]
 * Strengthened launch confirmation requires ALL of:
 *   1. Filtered altitude > 10m (1000cm)
 *   2. Vertical speed > 5 m/s (500 cm/s)
 * This prevents false launch from barometric drift or thermal expansion. */
#define LAUNCH_ALT_CM 1000
#define LAUNCH_SPEED_CMS 500

static state_event_t detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    /* [GND-TEST-01..04, DD-011] Poll serial for ground test commands.
     * Processed before the sample-rate gate so commands drain promptly. */
    char cmd_buf[64];
    if (hal_serial_readline(cmd_buf, sizeof(cmd_buf)))
        ground_test_handle_command(&ctx->gt, cmd_buf, ctx, now);
    ground_test_update(&ctx->gt, now);

    if (now - ctx->last_sample < 10)
        return SEVT_NONE;

    update_continuity_and_buzzer(ctx, now);

    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE; /* no altitude sample available yet */
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;

    /* Track speed on the pad for launch confirmation [DD-016] */
    uint32_t dt = (ctx->last_sample > 0) ? (ts - ctx->last_sample) : 10;
    if (dt > 0)
        ctx->pad_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    ctx->filtered_pressure = pp_last_filtered_pa(); /* for telemetry/debug */

    /* [GND-CAL-01] Continuously track atmospheric drift so altitude stays
     * near zero during extended pad time.  Uses a first-order IIR with
     * τ = 60 s.  A sub-Pa accumulator (units: Pa × 1000) prevents the
     * integer step from rounding to zero on each 20 ms tick.
     *
     *   α = dt_ms / (60000 + dt_ms)   ≈ 3.33 × 10⁻⁴ at 50 Hz
     *   step_acc += diff × α × 1000   (diff in Pa, acc in mPa)
     *   ground_pressure += acc / 1000  when ≥ 1 Pa accumulated */
    {
        int32_t gnd = pp_ground_pressure();
        int32_t gnd_diff = ctx->filtered_pressure - gnd;
        /* alpha scaled by 1,000,000: α×1e6 = dt×1e6 / (60000+dt) */
        int32_t alpha_ppm = ((int32_t)dt * 1000000) / (60000 + (int32_t)dt);
        /* Accumulate in mPa (Pa × 1000) */
        ctx->gnd_track_acc += (gnd_diff * alpha_ppm) / 1000;
        int32_t apply = ctx->gnd_track_acc / 1000;
        if (apply != 0) {
            pp_set_ground_pressure(gnd + apply);
            ctx->ground_pressure = pp_ground_pressure();
            ctx->gnd_track_acc -= apply * 1000;
        }
    }

    buf_add(ctx, 0, ctx->filtered_pressure, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = ts;

    bool alt_ok = altitude > LAUNCH_ALT_CM;
    bool speed_ok = ctx->pad_speed_cms > LAUNCH_SPEED_CMS;
    return (alt_ok && speed_ok) ? SEVT_LAUNCH : SEVT_NONE;
}

/* [FLT-ASC-01..06, FLT-APO-01..04, FLT-RATE-02, DD-013, DD-017]
 * DD-017: Arming gate — pyros arm only after max filtered speed exceeds
 *         threshold. The IIR filter (τ=500ms) attenuates measured speed
 *         by ~50% for short flights, so 10 m/s filtered ≈ 20 m/s true.
 *         Prevents false arming from barometric drift (~0 m/s filtered).
 * DD-013: Backup apogee timer — force apogee if not detected within
 *         backup_timer seconds after arming. Safety net for sensor failure. */
#define ARM_SPEED_CMS 1000 /* 10 m/s filtered ≈ 20 m/s true [DD-017] */

/* [DD-017] Arming requires confirmed motor burn: peak speed > threshold,
 * coast phase entered (speed decreasing but still positive). */
static bool arming_gate_met(const flight_context_t *ctx) {
    return !ctx->pyros_armed && ctx->max_speed_cms >= ARM_SPEED_CMS && ctx->vertical_speed_cms < 1000 &&
           ctx->vertical_speed_cms >= 0;
}

/* [DD-013] Backup apogee timer: force apogee after configurable timeout. */
static bool backup_apogee_expired(const flight_context_t *ctx, uint32_t now) {
    uint32_t timer_s = ctx->config.backup_timer;
    return timer_s > 0 && ctx->armed_time > 0 && (now - ctx->armed_time) >= timer_s * 1000;
}

static state_event_t detect_ascent(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;
    uint32_t dt = ts - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;
    ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;

    /* Track peak speed for arming gate [DD-017] */
    if (ctx->vertical_speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = ctx->vertical_speed_cms;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, ASCENT);
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].under_thrust =
        ctx->under_thrust ? 1 : 0;

    if (altitude > ctx->max_altitude)
        ctx->max_altitude = altitude;
    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    if (arming_gate_met(ctx))
        return SEVT_ARMED;

    if (ctx->pyros_armed && !ctx->apogee_detected) {
        if (ctx->vertical_speed_cms <= 0)
            return SEVT_APOGEE;
        if (backup_apogee_expired(ctx, now))
            return SEVT_APOGEE;
    }
    return SEVT_NONE;
}

/* [FLT-LAND-01..03, FLT-RATE-03, DD-015]
 * DD-015: Landing timeout — if descent has lasted landing_timeout seconds
 * and speed is below 5 m/s, force landing regardless of AGL altitude.
 * Handles landing at elevations above the launch pad (mesa, hillside). */
#define LANDING_SPEED_CMS 500 /* 5 m/s — slow enough to be "landed" */

static state_event_t detect_descent(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;
    uint32_t dt = ts - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, DESCENT);
    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    check_refire(ctx, now);

    /* Normal landing: stable altitude + low speed + near ground */
    bool altitude_stable = abs(altitude - ctx->last_altitude) < 100;
    bool speed_low = abs(ctx->vertical_speed_cms) < 200;
    bool near_ground = altitude < 3000;

    if (altitude_stable && speed_low && near_ground) {
        if (ctx->landing_stable_since == 0)
            ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            ctx->last_altitude = altitude;
            ctx->last_sample = now;
            return SEVT_LANDING;
        }
    } else {
        ctx->landing_stable_since = 0;
    }

    /* [DD-015] Landing timeout: force landing if descent > N seconds
     * and speed is low enough to be on the ground. Handles elevation
     * mismatch (landed at higher altitude than launch site). */
    uint32_t timeout_s = ctx->config.landing_timeout;
    if (timeout_s > 0 && ctx->descent_start_time > 0 && (now - ctx->descent_start_time) >= timeout_s * 1000 &&
        abs(ctx->vertical_speed_cms) < LANDING_SPEED_CMS) {
        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return SEVT_LANDING;
    }

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return SEVT_NONE;
}

/* [FLT-LAND-06, FLT-RATE-04] */
static state_event_t detect_landed(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    /* Landed: just log occasional samples for telemetry */
    ctx->filtered_pressure = pp_last_filtered_pa();
    /* Only add to ring buffer every 1 second to prevent overwriting events */
    if (now - ctx->last_sample >= 1000)
        buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, LANDED);
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_sample = sample.timestamp_ms;
    return SEVT_NONE;
}

/* ── Transition actions ───────────────────────────────────────────── */

static void action_cal_init(flight_context_t *ctx, uint32_t now) {
    (void)now;
    pp_start_cal(); /* pressure_processing layer handles calibration */
}

static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->ground_pressure = pp_ground_pressure();
    ctx->filtered_pressure = ctx->ground_pressure;
    /* last_altitude is already 0 from flight_init() — correct for ground.
     * No need to set last_sample or filter state — pressure_processing
     * owns the filter and ring buffer now. Altitude samples will arrive
     * in chronological order via pp_read(). */
}

/* [FLT-LAUNCH-03..05] Backdate launch time to first sample above 50cm */
static void action_launch(flight_context_t *ctx, uint32_t now) {
    buzzer_stop();
    ctx->launch_time = now;
    for (int i = ctx->buf_count - 1; i >= 0; i--) {
        uint16_t idx = (ctx->buf_head - 1 - i + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE;
        if (ctx->flight_buffer[idx].altitude_cm <= 50) {
            ctx->launch_time = now - (ctx->buf_count - 1 - i) * 10;
            break;
        }
    }
    /* [GND-CAL-02] Snap ground pressure to the current filtered pressure so
     * T+0 altitude is exactly 0 cm regardless of any residual tracker error.
     * ctx->ground_pressure (written to the log header) now records the true
     * atmospheric pressure at the moment of launch. */
    pp_set_ground_pressure(ctx->filtered_pressure);
    ctx->ground_pressure = ctx->filtered_pressure;
    ctx->last_altitude = 0;

    /* v2-9: start log BEFORE tagging — LAUNCH sample is PAD_IDLE state
     * (below the >= ASCENT guard in buf_tag_event), so emit it directly.
     * time_ms = 0 = T+0 relative to launch. */
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(0, ctx->filtered_pressure, 0, ASCENT, 0, EVT_LAUNCH);
    buf_tag_event(ctx, EVT_LAUNCH); /* tags ring buffer for flight_save_csv() */
}

/* [FLT-ASC-05, DD-017] Record armed_time for backup timer */
static void action_armed(flight_context_t *ctx, uint32_t now) {
    ctx->pyros_armed = true;
    ctx->armed_time = now; /* [DD-013] start backup timer */
    buf_tag_event(ctx, EVT_ARMED);
}

/* [FLT-APO-03, DD-014] Force CSV flush at apogee */
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = now;
    ctx->descent_start_time = now; /* [DD-015] start landing timeout */
    buf_tag_event(ctx, EVT_APOGEE);
    telemetry_apogee(ctx->max_altitude, now - ctx->launch_time);
    try_fire_pyros(ctx, now);
    /* [DD-014] hal_log async task flushes to flash every 200ms — no
     * explicit apogee flush needed. hal_log_sample() already wrote the
     * APOGEE event line immediately above. */
}

/* [FLT-LAND-05, BUZ-03, DAT-06] */
static void action_landing(flight_context_t *ctx, uint32_t now) {
    buf_tag_event(ctx, EVT_LANDING);
    telemetry_landing(ctx->max_altitude, now - ctx->launch_time);
    buzzer_set_altitude(cm_to_units(ctx->max_altitude, ctx->config.units));
    ctx->landed_beep_started = true;
    ctx->csv_saved = true;
    /* v2-9: finalize flight log — flush remaining buffer to flash */
    hal_log_stop();
}

/* ── State machine ────────────────────────────────────────────────── */

static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_SETTLE] = detect_boot_settle,
    [BOOT_CONTINUITY] = detect_boot_continuity,
    [BOOT_CALIBRATE] = detect_boot_calibrate,
    [PAD_IDLE] = detect_pad_idle,
    [ASCENT] = detect_ascent,
    [DESCENT] = detect_descent,
    [LANDED] = detect_landed,
};

static const transition_t transitions[] = {
    {BOOT_SETTLE, SEVT_TIMER, BOOT_CONTINUITY, NULL},
    {BOOT_CONTINUITY, SEVT_DONE, BOOT_CALIBRATE, action_cal_init},
    {BOOT_CALIBRATE, SEVT_CAL_DONE, PAD_IDLE, action_ground_cal},
    {PAD_IDLE, SEVT_LAUNCH, ASCENT, action_launch},
    {ASCENT, SEVT_ARMED, ASCENT, action_armed},
    {ASCENT, SEVT_APOGEE, DESCENT, action_apogee},
    {DESCENT, SEVT_LANDING, LANDED, action_landing},
};

#define NUM_TRANSITIONS (sizeof(transitions) / sizeof(transitions[0]))

flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state >= STATE_COUNT)
        return PAD_IDLE;

    detect_fn detect = detectors[ctx->current_state];
    if (!detect)
        return ctx->current_state;

    state_event_t evt = detect(ctx, now);
    if (evt == SEVT_NONE)
        return ctx->current_state;

    for (int i = 0; i < (int)NUM_TRANSITIONS; i++) {
        if (transitions[i].from == ctx->current_state && transitions[i].event == evt) {
            if (transitions[i].action)
                transitions[i].action(ctx, now);
            return transitions[i].to;
        }
    }
    return ctx->current_state;
}

/* ── CSV export ───────────────────────────────────────────────────── */

static const char *event_name(uint8_t evt) {
    switch (evt) {
    case EVT_LAUNCH:
        return "LAUNCH";
    case EVT_ARMED:
        return "ARMED";
    case EVT_APOGEE:
        return "APOGEE";
    case EVT_PYRO1_FIRE:
        return "PYRO1";
    case EVT_PYRO2_FIRE:
        return "PYRO2";
    case EVT_LANDING:
        return "LANDING";
    default:
        return "";
    }
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
    case PYRO_MODE_DELAY:
        return "delay";
    case PYRO_MODE_AGL:
        return "agl";
    case PYRO_MODE_FALLEN:
        return "fallen";
    case PYRO_MODE_SPEED:
        return "speed";
    default:
        return "none";
    }
}

/* [DAT-06, DAT-07] Batch CSV export (fallback, used by simulator) */
int flight_save_csv(flight_context_t *ctx) {
    if (ctx->buf_count == 0)
        return -1;
    hal_file_t *f = hal_fs_open("flight.csv", false);
    if (!f)
        return -1;

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "# Pyro MK1B Flight Data\n# ID: %.8s\n# Name: %.8s\n"
                     "# Pyro1: %s %u\n# Pyro2: %s %u\n"
                     "# Units: %s\n# Ground Pa: %ld\n# Max Alt cm: %ld\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
                     ctx->config.id, ctx->config.name, mode_name(ctx->config.pyro1_mode), ctx->config.pyro1_value,
                     mode_name(ctx->config.pyro2_mode), ctx->config.pyro2_value,
                     ctx->config.units == 2   ? "ft"
                     : ctx->config.units == 1 ? "m"
                                              : "cm",
                     (long)ctx->ground_pressure, (long)ctx->max_altitude);
    hal_fs_write(f, line, n);

    uint16_t idx = ctx->buf_tail;
    for (int i = 0; i < ctx->buf_count; i++) {
        flight_sample_t *s = &ctx->flight_buffer[idx];
        n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)s->time_ms, (long)s->pressure_pa,
                     (long)s->altitude_cm, s->state, s->under_thrust, event_name(s->event));
        hal_fs_write(f, line, n);
        idx = (idx + 1) % FLIGHT_BUF_SIZE;
    }
    hal_fs_close(f);
    return 0;
}

/* ── Init and output ──────────────────────────────────────────────── */

/* Global flight context pointer for runtime config reload and HTTP server access */
static flight_context_t *g_flight_ctx = NULL;

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    config_set_defaults(&ctx->config);

    /* Hardware init formerly in detect_boot_init() — runs once before
     * the main loop so there is no separate BOOT_INIT state. */
    hal_config_load(&ctx->config);
    telemetry_init(&ctx->config);
    buzzer_init();
    pp_init();
    hal_pressure_init();
    hal_pyro_init();
    ctx->boot_timer = hal_time_ms();

    ctx->current_state = BOOT_SETTLE;
    ground_test_init(&ctx->gt);

    /* Store global reference for config reload and HTTP server access */
    g_flight_ctx = ctx;
}

/* ── Config reload (runtime config update) ────────────────────────── */

int flight_config_reload(flight_context_t *ctx) {
    /* Safety check: only allow reload in PAD_IDLE state */
    if (ctx->current_state != PAD_IDLE) {
        return -1; /* Rejected: not in safe state */
    }

    /* Load config from persistent storage */
    config_t new_config;
    config_set_defaults(&new_config);
    if (hal_config_load(&new_config) < 0) {
        return -2; /* Rejected: load failed */
    }

    /* Validate critical fields to prevent invalid configurations */
    if (new_config.pyro1_mode > PYRO_MODE_DELAY || new_config.pyro2_mode > PYRO_MODE_DELAY) {
        return -3; /* Rejected: invalid pyro mode */
    }
    if (new_config.units > 2) {
        return -3; /* Rejected: invalid units */
    }

    /* Apply new configuration */
    ctx->config = new_config;

    /* Reinitialize telemetry with new config (updates headers, format, etc.) */
    telemetry_init(&ctx->config);

    return 0; /* Success */
}

flight_context_t *flight_get_context(void) {
    return g_flight_ctx;
}

flight_state_t flight_get_state(void) {
    return g_flight_ctx ? g_flight_ctx->current_state : BOOT_SETTLE;
}

/* Map flight_state_t to the 0-3 telemetry state_id */
static uint8_t state_to_telem_id(flight_state_t state) {
    switch (state) {
    case PAD_IDLE:
        return 0;
    case ASCENT:
        return 1;
    case DESCENT:
        return 2;
    case LANDED:
        return 3;
    default:
        return 0;
    }
}

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    /* Buzzer is now autonomous — driven by hal_tasks_tick(), no call needed here. */

    if (ctx->current_state >= PAD_IDLE) {
        uint32_t interval = (ctx->current_state == ASCENT || ctx->current_state == DESCENT) ? 100 : 1000;
        if (now - ctx->last_telemetry >= interval) {
            uint32_t flight_time = (ctx->current_state != PAD_IDLE) ? (now - ctx->launch_time) : 0;
            telemetry_snapshot_t snap = {
                .seq = ctx->telemetry_seq,
                .state_id = state_to_telem_id(ctx->current_state),
                .thrust = (ctx->current_state == ASCENT && ctx->under_thrust) ? 1u : 0u,
                .alt_cm = ctx->last_altitude,
                .max_alt_cm = ctx->max_altitude,
                .speed_cms = ctx->vertical_speed_cms,
                .press_pa = ctx->filtered_pressure,
                .p1_adc = ctx->pyro1_adc,
                .p2_adc = ctx->pyro2_adc,
                .time_ms = flight_time,
                .flags = 0,
            };
            if (ctx->pyro1_continuity_good)
                snap.flags |= TELEM_FLAG_P1_CONT;
            if (ctx->pyro2_continuity_good)
                snap.flags |= TELEM_FLAG_P2_CONT;
            if (ctx->pyro1_fired)
                snap.flags |= TELEM_FLAG_P1_FIRED;
            if (ctx->pyro2_fired)
                snap.flags |= TELEM_FLAG_P2_FIRED;
            if (ctx->pyros_armed)
                snap.flags |= TELEM_FLAG_ARMED;
            if (ctx->apogee_detected)
                snap.flags |= TELEM_FLAG_APOGEE;
            telemetry_state(&snap);
            ctx->telemetry_seq++;
            ctx->last_telemetry = now;
        }
    }
}
