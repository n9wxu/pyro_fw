/*
 * Heights above a pad in the 1976 standard's own air: what a unit test feeds
 * a module that takes pressures or altitudes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef STANDARD_AIR_H
#define STANDARD_AIR_H

#include "../src/atmosphere.h"

static inline float standard_pressure_above_pa(float pad_pa, float height_m) {
    return atmos_pressure_pa(atmos_altitude_m(pad_pa) + height_m);
}

static inline float standard_height_above_m(float pressure_pa, float pad_pa) {
    return atmos_altitude_m(pressure_pa) - atmos_altitude_m(pad_pa);
}

#endif
