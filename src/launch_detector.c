/*
 * See launch_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "launch_detector.h"
#include "atmosphere.h"

#define LAUNCH_HEIGHT_M 30.48f /* 100 ft */
#define LAUNCH_SPEED_MS 5.0f

bool launch_detected(const pp_sample_t *s, float pad_pa) {
    bool high = s->pressure_pa < atmos_pressure_above_pa(pad_pa, LAUNCH_HEIGHT_M);
    bool climbing = -s->rate * atmos_scale_height_m(s->pressure_pa) > LAUNCH_SPEED_MS;
    return high && climbing;
}
