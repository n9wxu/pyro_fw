/*
 * See fire_control.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fire_control.h"
#include "atmosphere.h"
#include "config.h"
#include <math.h>

#define CHARGE_GUARD_MS 2000u /* [PYR-MODE-06] */
#define G0 9.80665f

void fire_control_reset(fire_state_t *s) {
    *s = (fire_state_t){0};
}

/* ── What the rules may believe after a charge [PYR-MODE-06] ───────── */

typedef struct {
    float pressure_pa;
    float rate;
} believed_t;

static bool guard_running(const fire_state_t *s, const fire_inputs_t *in) {
    return s->charge_guard && in->now_ms - s->guard_from_ms < CHARGE_GUARD_MS;
}

static believed_t believed_state(const fire_state_t *s, const fire_inputs_t *in) {
    believed_t b = {in->pressure_pa, in->rate};
    if (!guard_running(s, in))
        return b;
    float dt = (float)(in->now_ms - s->guard_from_ms) / 1000.0f;
    float fall_curve = G0 / atmos_scale_height_m(in->pressure_pa);
    float free_fall_rate = s->guard_rate + fall_curve * dt;
    float free_fall_pa = expf(s->guard_log_pressure + s->guard_rate * dt + 0.5f * fall_curve * dt * dt);
    if (b.rate > free_fall_rate)
        b.rate = free_fall_rate;
    if (b.pressure_pa > free_fall_pa)
        b.pressure_pa = free_fall_pa;
    return b;
}

static float true_descent_ms(const believed_t *b) {
    return b->rate * atmos_scale_height_m(b->pressure_pa);
}

float fire_control_descent_speed_ms(const fire_state_t *s, const fire_inputs_t *in) {
    believed_t b = believed_state(s, in);
    return true_descent_ms(&b) * atmos_pad_air_ratio(b.pressure_pa, in->pad_pa);
}

/* ── First fires: each channel's own trigger [PYR-MODE-01..05] ─────── */

static bool trigger_met(const fire_plan_t *plan, int i, const fire_inputs_t *in, const believed_t *b) {
    switch (plan->mode[i]) {
    case PYRO_MODE_DELAY:
        return in->now_ms - in->apogee_ms >= plan->trigger_delay_ms[i];
    case PYRO_MODE_AGL:
        return in->state_known && b->pressure_pa >= atmos_pressure_above_pa(in->pad_pa, plan->trigger_m[i]);
    case PYRO_MODE_FALLEN:
        return in->state_known &&
               b->pressure_pa >= atmos_pressure_pa(atmos_altitude_m(in->peak_pa) - plan->trigger_m[i]);
    case PYRO_MODE_SPEED:
        return in->state_known && true_descent_ms(b) >= plan->trigger_ms[i];
    default:
        return false;
    }
}

/* ── What each channel is owed ─────────────────────────────────────── */

static bool interval_passed(const fire_channel_t *c, const fire_plan_t *plan, uint32_t now_ms) {
    return now_ms - c->last_pulse_ms >= plan->refire_interval_ms;
}

static fire_kind_t owed(const fire_state_t *s, const fire_plan_t *plan, int i, const fire_inputs_t *in,
                        const believed_t *b, float descent_ms) {
    const fire_channel_t *c = &s->channel[i];
    if (!plan->enabled[i] || !in->apogee_declared)
        return FIRE_NONE;
    bool emergency = in->state_known && plan->emergency_ms > 0.0f && descent_ms >= plan->emergency_ms;
    if (!c->fired) {
        if (emergency)
            return FIRE_EMERGENCY;
        return trigger_met(plan, i, in, b) ? FIRE_FIRST : FIRE_NONE;
    }
    if (!interval_passed(c, plan, in->now_ms))
        return FIRE_NONE;
    if (emergency)
        return FIRE_EMERGENCY;
    bool too_fast = in->state_known && plan->refire_ms[i] > 0.0f && descent_ms > plan->refire_ms[i];
    return too_fast ? FIRE_REFIRE : FIRE_NONE;
}

/* ── Whose turn [PYR-DEPLOY-02] ────────────────────────────────────── */

/* A first fire goes before a re-fire. Between re-fires the channel that has
 * waited longer goes, so neither starves the other; on a tie, pyro 1. */
static int first_in_line(const fire_state_t *s, const fire_kind_t owes[2]) {
    if (owes[0] == FIRE_NONE)
        return owes[1] == FIRE_NONE ? -1 : 1;
    if (owes[1] == FIRE_NONE)
        return 0;
    bool first0 = !s->channel[0].fired, first1 = !s->channel[1].fired;
    if (first0 != first1)
        return first0 ? 0 : 1;
    if (first0)
        return 0;
    return (int32_t)(s->channel[1].last_pulse_ms - s->channel[0].last_pulse_ms) >= 0 ? 0 : 1;
}

static bool gap_passed(const fire_state_t *s, const fire_plan_t *plan, int i, uint32_t now_ms) {
    const fire_channel_t *other = &s->channel[1 - i];
    if (!other->fired)
        return true;
    return other->pulse_ended && now_ms - other->last_pulse_end_ms >= plan->fire_gap_ms;
}

static void note_pulse_end(fire_state_t *s, const fire_inputs_t *in) {
    if (s->active_channel == 0 || in->pulse_active)
        return;
    fire_channel_t *c = &s->channel[s->active_channel - 1];
    c->last_pulse_end_ms = in->now_ms;
    c->pulse_ended = true;
    s->active_channel = 0;
}

fire_command_t fire_control_step(fire_state_t *s, const fire_plan_t *plan, const fire_inputs_t *in) {
    const fire_command_t none = {0, FIRE_NONE};
    note_pulse_end(s, in);
    if (in->pulse_active)
        return none;

    believed_t b = believed_state(s, in);
    float descent_ms = true_descent_ms(&b) * atmos_pad_air_ratio(b.pressure_pa, in->pad_pa);
    fire_kind_t owes[2] = {owed(s, plan, 0, in, &b, descent_ms), owed(s, plan, 1, in, &b, descent_ms)};

    int i = first_in_line(s, owes);
    if (i < 0 || !gap_passed(s, plan, i, in->now_ms))
        return none;
    return (fire_command_t){(uint8_t)(i + 1), owes[i]};
}

void fire_control_pulse_started(fire_state_t *s, uint8_t channel, const fire_inputs_t *in) {
    if (channel != 1 && channel != 2)
        return;
    fire_channel_t *c = &s->channel[channel - 1];
    /* The guard keeps the state from before the first charge of a burst:
     * a later pulse inside the window sees pressure the first one raised. */
    if (!guard_running(s, in)) {
        s->guard_log_pressure = logf(in->pressure_pa);
        s->guard_rate = in->rate > 0.0f ? in->rate : 0.0f;
    } else {
        believed_t b = believed_state(s, in);
        s->guard_log_pressure = logf(b.pressure_pa);
        s->guard_rate = b.rate > 0.0f ? b.rate : 0.0f;
    }
    s->charge_guard = true;
    s->guard_from_ms = in->now_ms;
    c->fired = true;
    c->pulses++;
    c->last_pulse_ms = in->now_ms;
    c->pulse_ended = false;
    s->active_channel = channel;
}
