/*
 * The fire plan: the configuration's pyro fields in SI units, within the
 * board's limits [CFG-02, PYR-BOARD-01, PYR-BOARD-02, PYR-HEALTH-02].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FIRE_PLAN_H
#define FIRE_PLAN_H

#include "config.h"
#include "fire_control.h"
#include "hal.h"

typedef struct {
    fire_plan_t plan;
    bool refire_interval_limited; /* the configured value was outside the board's range */
    bool fire_gap_limited;
} fire_plan_result_t;

fire_plan_result_t fire_plan_from(const config_t *cfg, const hal_pyro_limits_t *limits, const bool pads_owned[2]);

/* A length or a speed in the configured units, as metres. */
float fire_plan_units_to_m(uint16_t value, uint8_t units);

#endif
