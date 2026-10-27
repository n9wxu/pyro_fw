/*
 * See launch_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "launch_detector.h"
#include "atmosphere.h"

#define LAUNCH_HEIGHT_M 15.0f

bool launch_detected(const pp_sample_t *s, float pad_pa) {
    return s->pressure_pa > 0.0f && s->pressure_pa < atmos_pressure_above_pa(pad_pa, LAUNCH_HEIGHT_M);
}
