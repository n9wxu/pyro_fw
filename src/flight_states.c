/*
 * See flight_states.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "beep_store.h"
#include "board_id.h"
#include "buzzer.h"
#include "fire_plan.h"
#include "flight_resume.h"
#include "pressure_processing.h"
#include "pyro_release.h"
#include <stdio.h>
#include <string.h>

/* A platform with no ground test switch. */
__attribute__((weak)) bool hal_ground_test_asserted(void) {
    return false;
}

static flight_context_t *g_flight_ctx;

typedef state_event_t (*detect_fn)(flight_context_t *ctx, uint32_t now);
typedef void (*action_fn)(flight_context_t *ctx, uint32_t now);

typedef struct {
    flight_state_t from;
    state_event_t event;
    flight_state_t to;
    action_fn action;
} transition_t;

static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_SETTLE] = flight_detect_boot_settle,
    [BOOT_SENSOR] = flight_detect_boot_sensor,
    [BOOT_CONTINUITY] = flight_detect_boot_continuity,
    [BOOT_CALIBRATE] = flight_detect_boot_calibrate,
    [PAD_IDLE] = flight_detect_pad_idle,
    [ASCENT] = flight_detect_ascent,
    [FALLING] = flight_detect_falling,
    [DROGUE_DESCENT] = flight_detect_drogue_descent,
    [CHUTE_DESCENT] = flight_detect_chute_descent,
    [LANDED] = flight_detect_landed,
    [FAULT] = flight_detect_fault,
    [GROUND_TEST] = flight_detect_ground_test,
};

static const transition_t transitions[] = {
    {BOOT_SETTLE, SEVT_TIMER, BOOT_SENSOR, NULL},
    {BOOT_SENSOR, SEVT_DONE, BOOT_CONTINUITY, NULL},
    {BOOT_SENSOR, SEVT_RESUME_ASCENT, ASCENT, NULL},
    {BOOT_SENSOR, SEVT_RESUME_DESCENT, FALLING, flight_action_resumed_descent},
    {BOOT_SENSOR, SEVT_FAULT, FAULT, flight_action_fault},
    {BOOT_CONTINUITY, SEVT_DONE, BOOT_CALIBRATE, flight_action_start_calibration},
    {BOOT_CONTINUITY, SEVT_GROUND_TEST, GROUND_TEST, flight_action_ground_test},
    {BOOT_CALIBRATE, SEVT_FAULT, FAULT, flight_action_fault},
    {BOOT_CALIBRATE, SEVT_CAL_DONE, PAD_IDLE, flight_action_on_pad},
    {PAD_IDLE, SEVT_LAUNCH, ASCENT, flight_action_launch},
    {ASCENT, SEVT_ARMED, ASCENT, flight_action_armed},
    {ASCENT, SEVT_APOGEE, FALLING, flight_action_apogee},
    {FALLING, SEVT_DROGUE, DROGUE_DESCENT, NULL},
    {FALLING, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    {DROGUE_DESCENT, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    {DROGUE_DESCENT, SEVT_FREEFALL, FALLING, NULL},
    {FALLING, SEVT_LANDING, LANDED, flight_action_landing},
    {DROGUE_DESCENT, SEVT_LANDING, LANDED, flight_action_landing},
    {CHUTE_DESCENT, SEVT_LANDING, LANDED, flight_action_landing},
};
#define NUM_TRANSITIONS (int)(sizeof(transitions) / sizeof(transitions[0]))

static flight_state_t step(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state >= STATE_COUNT)
        return PAD_IDLE;
    state_event_t event = detectors[ctx->current_state](ctx, now);
    if (event == SEVT_NONE)
        return ctx->current_state;
    for (int i = 0; i < NUM_TRANSITIONS; i++) {
        if (transitions[i].from != ctx->current_state || transitions[i].event != event)
            continue;
        if (transitions[i].action)
            transitions[i].action(ctx, now);
        return transitions[i].to;
    }
    return ctx->current_state;
}

/* [FLT-RATE-06] The collector hands on more samples than there are loops.
 * Each is a step of its own, in the loop it arrived in: taken one a loop,
 * they queue, and every decision is made on old data. */
#define STEPS_A_LOOP_MAX 8

flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    for (int steps = 0; steps < STEPS_A_LOOP_MAX; steps++) {
        int waiting = pp_available();
        ctx->current_state = step(ctx, now);
        bool took_one = pp_available() < waiting;
        if (!took_one || pp_available() == 0)
            break;
    }
    return ctx->current_state;
}

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    config_set_defaults(&ctx->config);
    hal_config_load(&ctx->config);
    buzzer_init();
    beep_store_load(NULL, 0);
    pp_init();
    pp_obey(estimator_index(ctx->config.estimator));
    ctx->reset_cause = (uint8_t)hal_reset_cause();
    hal_pressure_init();
    int sensor = hal_pressure_sensor();
    ctx->sensor_type = sensor < 0 ? SENSOR_PENDING : (uint8_t)sensor;
    ctx->storage_ok = hal_fs_healthy();
    hal_pyro_init();
    ctx->boot_timer = hal_time_ms();
    ctx->current_state = BOOT_SETTLE;
    g_flight_ctx = ctx;
}

flight_context_t *flight_get_context(void) {
    return g_flight_ctx;
}

flight_state_t flight_get_state(void) {
    return g_flight_ctx ? g_flight_ctx->current_state : BOOT_SETTLE;
}

/* ── Shared by the detectors ──────────────────────────────────────── */

bool flight_grounded_on_usb(const flight_context_t *ctx) {
    return ctx->usb_attached && !ctx->test_mode;
}

bool flight_state_is_airborne(flight_state_t state) {
    return state == ASCENT || state == FALLING || state == DROGUE_DESCENT || state == CHUTE_DESCENT;
}

static bool state_is_logged(flight_state_t state) {
    return flight_state_is_airborne(state) || state == LANDED;
}

void flight_make_plan(flight_context_t *ctx) {
    hal_pyro_limits_t limits;
    hal_pyro_limits(&limits);
    bool pads_owned[2] = {!pyro_release_is_released(1), !pyro_release_is_released(2)};
    fire_plan_result_t r = fire_plan_from(&ctx->config, &limits, pads_owned);
    ctx->plan = r.plan;
    ctx->refire_interval_limited = r.refire_interval_limited;
    ctx->fire_gap_limited = r.fire_gap_limited;
}

void flight_read_pyro_health(flight_context_t *ctx) {
    hal_continuity_t reading[2];
    hal_pyro_sample();
    hal_pyro_get(1, &reading[0]);
    hal_pyro_get(2, &reading[1]);
    for (int i = 0; i < 2; i++) {
        ctx->channel_ready[i] = reading[i].good;
        ctx->channel_adc[i] = reading[i].raw_adc;
    }
    ctx->diag = (uint16_t)((ctx->diag & ~DIAG_PYRO_ANY) | pad_check_pyro_faults(reading, ctx->plan.enabled));
}

static flight_sample_t *newest_row(flight_context_t *ctx) {
    return &ctx->flight_buffer[(ctx->buf_head + FLIGHT_BUF_SIZE - 1u) % FLIGHT_BUF_SIZE];
}

void flight_take_sample(flight_context_t *ctx, const pp_sample_t *s, flight_state_t state) {
    ctx->last_sample = s->timestamp_ms;
    ctx->pressure_pa = (int32_t)(s->pressure_pa + 0.5f);
    ctx->altitude_cm = s->altitude_cm;
    ctx->speed_cms = s->speed_cms;

    uint32_t flight_ms = state == PAD_IDLE ? 0u : s->timestamp_ms - ctx->launch_time;
    if (state_is_logged(state))
        hal_log_sample(flight_ms, ctx->pressure_pa, s->altitude_cm, (uint8_t)state, ctx->under_thrust, EVT_NONE);
    if (flight_state_is_airborne(state))
        flight_follow_estimators(ctx, s, flight_ms);

    flight_sample_t *row = &ctx->flight_buffer[ctx->buf_head];
    *row = (flight_sample_t){flight_ms, ctx->pressure_pa, s->altitude_cm, (uint8_t)state, ctx->under_thrust, EVT_NONE};
    ctx->buf_head = (uint16_t)((ctx->buf_head + 1u) % FLIGHT_BUF_SIZE);
    if (ctx->buf_count < FLIGHT_BUF_SIZE)
        ctx->buf_count++;
}

/* [DAT-03] An event is a row at the newest sample's time. */
void flight_log_event(flight_context_t *ctx, uint8_t event) {
    flight_sample_t *row = newest_row(ctx);
    row->event = event;
    if (hal_log_active() && state_is_logged((flight_state_t)row->state))
        hal_log_sample(row->time_ms, row->pressure_pa, row->altitude_cm, row->state, row->under_thrust, event);
}

/* [SNS-PRES-10, SNS-PRES-11, SNS-REC-01] Recorded once each time, and left
 * alone: nothing recovers a sensor in flight. */
void flight_log_peak(const flight_context_t *ctx) {
    int32_t peak_pa = ctx->peak_pa > 0.0f ? (int32_t)(ctx->peak_pa + 0.5f) : ctx->pressure_pa;
    hal_log_sample(ctx->last_sample - ctx->launch_time, peak_pa, ctx->max_altitude_cm, (uint8_t)ctx->current_state, 0,
                   ctx->peak_lower_bound ? EVT_PEAK_AT_LEAST : EVT_PEAK);
}

void flight_note_sensor(flight_context_t *ctx, const pp_sample_t *s, uint32_t now) {
    (void)now;
    ctx->sensor_lost = false;
    if (!s->sensor_stuck) {
        ctx->sensor_stuck = false;
        return;
    }
    if (ctx->sensor_stuck)
        return;
    ctx->sensor_stuck = true;
    ctx->diag |= DIAG_SENSOR_STUCK;
    flight_log_event(ctx, EVT_SENSOR_STUCK);
    hal_telemetry_send("!SENSOR STUCK\r\n");
}

#define SENSOR_LOST_MS 500u

void flight_note_no_sample(flight_context_t *ctx, uint32_t now) {
    if (ctx->sensor_lost || now - ctx->last_sample < SENSOR_LOST_MS)
        return;
    ctx->sensor_lost = true;
    ctx->diag |= DIAG_SENSOR_LOST;
    flight_log_event(ctx, EVT_SENSOR_LOST);
    hal_telemetry_send("!SENSOR LOST\r\n");
}

void flight_say_fault(void) {
    beep_spec_t spec = beep_for(BR_GENERAL_FAULT);
    buzzer_play_spec(&spec, beep_codes_active(beep_store_current())->gap_ms, 0);
}

int32_t flight_to_units(int32_t cm, uint8_t units) {
    switch (units) {
    case 1:
        return cm / 100;
    case 2:
        return cm * 100 / 3048;
    default:
        return cm;
    }
}

uint32_t flight_elapsed_ms(const flight_context_t *ctx, uint32_t now) {
    if (flight_state_is_airborne(ctx->current_state))
        return now - ctx->launch_time;
    if (ctx->current_state == LANDED)
        return ctx->landing_time - ctx->launch_time;
    return 0;
}

const char *flight_resume_text(const flight_context_t *ctx) {
    if (ctx->resume == RESUME_NOT_FLYING)
        return not_resumed_name((not_resumed_t)ctx->not_resumed);
    return resume_verdict_name((resume_verdict_t)ctx->resume);
}

/* ── The simulator's export ───────────────────────────────────────── */

int flight_save_csv(flight_context_t *ctx) {
    if (ctx->buf_count == 0)
        return -1;
    hal_file_t *f = hal_fs_open("flight.csv", false);
    if (!f)
        return -1;
    static const char *const unit_names[] = {"cm", "m", "ft"};
    char line[256];
    int n = snprintf(line, sizeof(line),
                     "# " PYRO_BOARD_NAME " Flight Data\n# ID: %.8s\n# Name: %.8s\n"
                     "# Pyro1: %s %u\n# Pyro2: %s %u\n"
                     "# Units: %s\n# Ground Pa: %ld\n# Max Alt cm: %ld\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
                     ctx->config.id, ctx->config.name, config_mode_name(ctx->config.pyro1_mode),
                     ctx->config.pyro1_value, config_mode_name(ctx->config.pyro2_mode), ctx->config.pyro2_value,
                     unit_names[ctx->config.units <= 2 ? ctx->config.units : 0], (long)ctx->ground_pressure,
                     (long)ctx->max_altitude_cm);
    hal_fs_write(f, line, n);
    uint16_t idx = (uint16_t)((ctx->buf_head + FLIGHT_BUF_SIZE - ctx->buf_count) % FLIGHT_BUF_SIZE);
    for (int i = 0; i < ctx->buf_count; i++) {
        const flight_sample_t *row = &ctx->flight_buffer[idx];
        n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)row->time_ms, (long)row->pressure_pa,
                     (long)row->altitude_cm, row->state, row->under_thrust, flight_event_name(row->event));
        hal_fs_write(f, line, n);
        idx = (uint16_t)((idx + 1u) % FLIGHT_BUF_SIZE);
    }
    hal_fs_close(f);
    return 0;
}
