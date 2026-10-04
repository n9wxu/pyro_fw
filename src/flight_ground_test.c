/*
 * Ground test mode [GND-TEST-05..13, DD-071, DD-087].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "buzzer.h"
#include <stdio.h>

void flight_action_ground_test(flight_context_t *ctx, uint32_t now) {
    gt_seq_begin(&ctx->gt_seq, ctx->plan.enabled[0], ctx->plan.enabled[1], hal_ground_test_asserted(), now);
    char line[40];
    snprintf(line, sizeof(line), "!GT MODE p1=%s p2=%s\r\n", ctx->plan.enabled[0] ? "on" : "off",
             ctx->plan.enabled[1] ? "on" : "off");
    hal_telemetry_send(line);
}

/* [PYR-DEPLOY-02] The quiet time between channels holds in ground test too. */
static bool gap_passed(const flight_context_t *ctx, uint8_t channel, uint32_t now) {
    const fire_channel_t *other = &ctx->fire.channel[2 - channel];
    return !other->fired || (other->pulse_ended && now - other->last_pulse_end_ms >= ctx->plan.fire_gap_ms);
}

/* With no apogee the fire rules ask for nothing: the step only notes when a
 * pulse ends, which the gap is measured from. */
static fire_inputs_t note_pulse_end(flight_context_t *ctx, uint32_t now) {
    fire_inputs_t in = flight_fire_inputs(ctx, NULL, now);
    (void)fire_control_step(&ctx->fire, &ctx->plan, &in);
    return in;
}

/* [GND-TEST-13] Delivered on command: no health reading withholds it. */
static gt_fire_t fire_on_command(flight_context_t *ctx, uint8_t channel, const fire_inputs_t *in) {
    if (in->pulse_active || !gap_passed(ctx, channel, in->now_ms))
        return GT_FIRE_BUSY;
    hal_pyro_fire(channel);
    bool energised = hal_pyro_is_firing();
    fire_control_pulse_started(&ctx->fire, channel, in);
    return energised ? GT_FIRE_ENERGISED : GT_FIRE_FAULT;
}

state_event_t flight_detect_ground_test(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_health_check > 1000u) {
        ctx->last_health_check = now;
        flight_read_pyro_health(ctx);
    }
    fire_inputs_t in = note_pulse_end(ctx, now);
    gt_phase_t was = ctx->gt_seq.phase;
    gt_action_t action = gt_seq_step(&ctx->gt_seq, hal_ground_test_asserted(), now);
    if (action.sound != GT_SOUND_NONE)
        buzzer_play_ground_test(action.sound);

    char line[40];
    if (action.fire) {
        gt_fire_t result = fire_on_command(ctx, action.fire, &in);
        gt_seq_fired(&ctx->gt_seq, action.fire, result, now);
        if (result != GT_FIRE_BUSY) {
            snprintf(line, sizeof(line), "!GT FIRE %u %s\r\n", (unsigned)action.fire,
                     result == GT_FIRE_ENERGISED ? "fired" : "fault");
            hal_telemetry_send(line);
        }
    }
    if (ctx->gt_seq.phase != was) {
        snprintf(line, sizeof(line), "!GT %s\r\n", gt_seq_phase_name(ctx->gt_seq.phase));
        hal_telemetry_send(line);
    }
    return SEVT_NONE;
}
