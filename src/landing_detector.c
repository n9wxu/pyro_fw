/*
 * See landing_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "landing_detector.h"
#include "atmosphere.h"
#include "hold.h"

#define STILL_MS 2.0f /* a main descends at 3 to 6 m/s: slower than that is still */
#define STILL_HOLD_MS 1000u
#define NEAR_GROUND_M 30.0f

bool landing_detected(landing_detector_t *d, const pp_sample_t *s, float pad_pa, uint32_t descent_ms,
                      uint32_t timeout_ms) {
    float speed_ms = s->rate * atmos_scale_height_m(s->pressure_pa);
    bool still = !s->suspect && speed_ms < STILL_MS && speed_ms > -STILL_MS;
    bool near_ground = s->pressure_pa > atmos_pressure_above_pa(pad_pa, NEAR_GROUND_M);
    bool timed_out = timeout_ms > 0 && descent_ms >= timeout_ms;

    bool landed_near = held_for(still && near_ground, &d->near_ground_since, s->timestamp_ms, STILL_HOLD_MS);
    bool landed_late = held_for(still && timed_out, &d->timed_out_since, s->timestamp_ms, STILL_HOLD_MS);
    return landed_near || landed_late;
}
