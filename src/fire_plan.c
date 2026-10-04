/*
 * See fire_plan.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fire_plan.h"

#define UNITS_CM 0
#define UNITS_M 1
#define UNITS_FT 2

float fire_plan_units_to_m(uint16_t value, uint8_t units) {
    switch (units) {
    case UNITS_M:
        return (float)value;
    case UNITS_FT:
        return (float)value * 0.3048f;
    case UNITS_CM:
    default:
        return (float)value / 100.0f;
    }
}

/* A configured 0 asks for the board's own default. */
static uint32_t within_limits(uint16_t configured, uint16_t board_default, uint16_t min, uint16_t max, bool *limited) {
    *limited = false;
    if (configured == 0)
        return board_default;
    if (configured < min || configured > max) {
        *limited = true;
        return configured < min ? min : max;
    }
    return configured;
}

static void plan_channel(fire_plan_t *plan, int i, uint8_t mode, uint16_t value, uint16_t refire_speed, uint8_t units,
                         bool pads_owned) {
    plan->enabled[i] = pads_owned && mode != PYRO_MODE_NONE;
    plan->mode[i] = mode;
    plan->trigger_m[i] = fire_plan_units_to_m(value, units);
    plan->trigger_ms[i] = fire_plan_units_to_m(value, units);
    plan->trigger_delay_ms[i] = (uint32_t)value * 1000u;
    plan->refire_ms[i] = fire_plan_units_to_m(refire_speed, units);
}

fire_plan_result_t fire_plan_from(const config_t *cfg, const hal_pyro_limits_t *limits, const bool pads_owned[2]) {
    fire_plan_result_t r = {0};
    plan_channel(&r.plan, 0, cfg->pyro1_mode, cfg->pyro1_value, cfg->pyro1_refire_speed, cfg->units, pads_owned[0]);
    plan_channel(&r.plan, 1, cfg->pyro2_mode, cfg->pyro2_value, cfg->pyro2_refire_speed, cfg->units, pads_owned[1]);
    r.plan.emergency_ms = fire_plan_units_to_m(cfg->emergency_fire_speed, cfg->units);
    r.plan.refire_interval_ms =
        within_limits(cfg->refire_interval, limits->refire_interval_default_ms, limits->refire_interval_min_ms,
                      limits->refire_interval_max_ms, &r.refire_interval_limited);
    r.plan.fire_gap_ms = within_limits(cfg->fire_gap, limits->fire_gap_default_ms, limits->fire_gap_min_ms,
                                       limits->fire_gap_max_ms, &r.fire_gap_limited);
    return r;
}
