/*
 * Descent: the fire rules, the descent phase and the landing
 * [PYR-MODE-01..06, PYR-FIRE-01, PYR-REFIRE-01, FLT-EMRG-01..05,
 * FLT-DESC-01, FLT-DESC-02, FLT-LAND-02..07, PYR-VERIFY-01].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "buzzer.h"
#include "pressure_processing.h"
#include <stddef.h>

#define VERIFY_FROM_MS 500u /* after a pulse starts, read whether the channel opened */

fire_inputs_t flight_fire_inputs(const flight_context_t *ctx, const pp_sample_t *s, uint32_t now_ms) {
    fire_inputs_t in = {
        .now_ms = now_ms,
        .apogee_declared = ctx->apogee_declared,
        .apogee_ms = ctx->apogee_time,
        .state_known = s != NULL && !s->suspect,
        .pressure_pa = s ? s->pressure_pa : (float)ctx->pressure_pa,
        .rate = s ? s->rate : 0.0f,
        .pad_pa = (float)ctx->ground_pressure,
        .peak_pa = ctx->peak_pa > 0.0f ? ctx->peak_pa : (float)ctx->pressure_pa,
        .pulse_active = hal_pyro_is_firing(),
    };
    return in;
}

static uint8_t pulse_event(uint8_t channel, bool first) {
    if (channel == 1)
        return first ? EVT_PYRO1_FIRE : EVT_PYRO1_REFIRE;
    return first ? EVT_PYRO2_FIRE : EVT_PYRO2_REFIRE;
}

static void note_fault(flight_context_t *ctx, int i) {
    if (ctx->channel_fault[i])
        return;
    ctx->channel_fault[i] = true;
    flight_log_event(ctx, i == 0 ? EVT_PYRO1_FAULT : EVT_PYRO2_FAULT);
}

void flight_deliver_pulse(flight_context_t *ctx, fire_command_t command, const fire_inputs_t *in) {
    int i = command.channel - 1;
    bool first = !ctx->fire.channel[i].fired;
    if (command.kind == FIRE_EMERGENCY && !ctx->emergency_fire) {
        ctx->emergency_fire = true;
        flight_log_event(ctx, EVT_EMERGENCY_FIRE);
    }
    hal_pyro_fire(command.channel);
    pp_note_pulse();
    bool energised = hal_pyro_is_firing();
    fire_control_pulse_started(&ctx->fire, command.channel, in);
    ctx->verify_done[i] = false;
    flight_log_event(ctx, pulse_event(command.channel, first));
    if (!energised)
        note_fault(ctx, i);
    telemetry_queue(&ctx->telemetry, (telemetry_event_t){TELEM_EVENT_FIRE, command.channel, ctx->altitude_cm,
                                                         in->now_ms - ctx->launch_time});
}

/* [PYR-FAULT-02, PYR-VERIFY-01] Records, on a board that can sense them.
 * Nothing depends on either. */
static void observe_pulses(flight_context_t *ctx, uint32_t now_ms) {
    for (int i = 0; i < 2; i++) {
        const fire_channel_t *c = &ctx->fire.channel[i];
        if (!c->fired)
            continue;
        if (hal_pyro_fault((uint8_t)(i + 1)))
            note_fault(ctx, i);
        if (ctx->verify_done[i] || now_ms - c->last_pulse_ms < VERIFY_FROM_MS || hal_pyro_is_firing())
            continue;
        hal_continuity_t reading = {0};
        hal_pyro_sample();
        hal_pyro_get((uint8_t)(i + 1), &reading);
        bool no_verdict_yet = !reading.good && !reading.open && !reading.shorted;
        if (no_verdict_yet)
            continue; /* the board's next check has not run: a reading from before the fire is not one */
        ctx->verify_done[i] = true;
        if (reading.good && !reading.open && !ctx->channel_stayed_closed[i]) {
            ctx->channel_stayed_closed[i] = true;
            flight_log_event(ctx, i == 0 ? EVT_PYRO1_NOPEN : EVT_PYRO2_NOPEN);
        }
    }
}

static void run_fire_rules(flight_context_t *ctx, const pp_sample_t *s, uint32_t now_ms) {
    fire_inputs_t in = flight_fire_inputs(ctx, s, now_ms);
    if (s)
        ctx->descent_speed_cms = (int32_t)(fire_control_descent_speed_ms(&ctx->fire, &in) * 100.0f);
    if (ctx->emergency_fire && ctx->plan.emergency_ms > 0.0f &&
        (float)ctx->descent_speed_cms < ctx->plan.emergency_ms * 100.0f)
        ctx->emergency_fire = false;
    fire_command_t command = fire_control_step(&ctx->fire, &ctx->plan, &in);
    if (command.kind != FIRE_NONE)
        flight_deliver_pulse(ctx, command, &in);
    observe_pulses(ctx, now_ms);
}

/* Every descent state takes the sample the same way; only the verdict on
 * the phase differs. With no sample the rules still run, so a DELAY whose
 * apogee is known fires on time [SYS-DEPLOY-04]. */
static bool descend(flight_context_t *ctx, uint32_t now, flight_state_t state, pp_sample_t *s) {
    if (!pp_read(s)) {
        flight_note_no_sample(ctx, now);
        run_fire_rules(ctx, NULL, now);
        return false;
    }
    flight_take_sample(ctx, s, state);
    flight_note_sensor(ctx, s, now);
    run_fire_rules(ctx, s, s->timestamp_ms);
    return true;
}

static bool landed(flight_context_t *ctx, const pp_sample_t *s, uint32_t now) {
    return landing_detected(&ctx->landing, s, (float)ctx->ground_pressure, now - ctx->descent_start_time,
                            (uint32_t)ctx->config.landing_timeout * 1000u);
}

static bool settled(flight_context_t *ctx, descent_band_t *band) {
    return descent_phase_settled(&ctx->descent, -ctx->descent_speed_cms, ctx->last_sample, band);
}

state_event_t flight_detect_falling(flight_context_t *ctx, uint32_t now) {
    pp_sample_t s;
    if (!descend(ctx, now, FALLING, &s))
        return SEVT_NONE;
    descent_band_t band = DESCENT_FAST;
    bool steady = settled(ctx, &band);
    if (landed(ctx, &s, now))
        return SEVT_LANDING;
    if (steady && band == DESCENT_UNDER_MAIN)
        return SEVT_CHUTE;
    return steady && band == DESCENT_UNDER_DROGUE ? SEVT_DROGUE : SEVT_NONE;
}

state_event_t flight_detect_drogue_descent(flight_context_t *ctx, uint32_t now) {
    pp_sample_t s;
    if (!descend(ctx, now, DROGUE_DESCENT, &s))
        return SEVT_NONE;
    descent_band_t band = DESCENT_FAST;
    bool steady = settled(ctx, &band);
    if (landed(ctx, &s, now))
        return SEVT_LANDING;
    if (steady && band == DESCENT_UNDER_MAIN)
        return SEVT_CHUTE;
    bool lost = descent_phase_lost_drogue(&ctx->descent, ctx->descent_speed_cms, ctx->last_sample);
    return lost ? SEVT_FREEFALL : SEVT_NONE;
}

state_event_t flight_detect_chute_descent(flight_context_t *ctx, uint32_t now) {
    pp_sample_t s;
    if (!descend(ctx, now, CHUTE_DESCENT, &s))
        return SEVT_NONE;
    return landed(ctx, &s, now) ? SEVT_LANDING : SEVT_NONE;
}

void flight_action_landing(flight_context_t *ctx, uint32_t now) {
    ctx->landing_time = now;
    flight_log_event(ctx, EVT_LANDING);
    telemetry_queue(&ctx->telemetry,
                    (telemetry_event_t){TELEM_EVENT_LANDING, 0, ctx->max_altitude_cm, now - ctx->launch_time});
    buzzer_play_altitude(flight_to_units(ctx->max_altitude_cm, ctx->config.units));
    hal_log_stop();
}

#define LANDED_ROW_MS 1000u

state_event_t flight_detect_landed(flight_context_t *ctx, uint32_t now) {
    (void)now;
    pp_sample_t s;
    if (!pp_read(&s))
        return SEVT_NONE;
    if (ctx->landed_row_ms != 0 && s.timestamp_ms - ctx->landed_row_ms < LANDED_ROW_MS) {
        ctx->last_sample = s.timestamp_ms;
        return SEVT_NONE;
    }
    flight_take_sample(ctx, &s, LANDED);
    ctx->landed_row_ms = s.timestamp_ms;
    return SEVT_NONE;
}
